"""HA-script entry points addressing explicit ESPHome device IDs."""

import asyncio

import voluptuous as vol
from homeassistant.const import EVENT_STATE_CHANGED
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.network import get_url
from homeassistant.util import slugify

from .playback import MonitorStreamView, PlaybackSessions

DOMAIN = "voice_pe_monitor"


def register_services(hass):
    def resolve(device_id, action):
        device = dr.async_get(hass).async_get(device_id)
        if device is not None:
            for entry_id in device.config_entries:
                entry = hass.config_entries.async_get_entry(entry_id)
                if (
                    entry
                    and entry.domain == "esphome"
                    and (name := entry.data.get("device_name"))
                ):
                    service = slugify(f"{name}_monitor_{action}")
                    if hass.services.has_service("esphome", service):
                        return name, service
        raise HomeAssistantError(
            "Update monitor firmware and select its ESPHome device"
        )

    async def native(device_id, action, data=None):
        _, service = resolve(device_id, action)
        await hass.services.async_call("esphome", service, data or {}, blocking=True)

    async def stop_native(device):
        await native(device, "stop")

    sessions = hass.data["voice_pe_monitor_sessions"] = PlaybackSessions(stop_native)
    hass.http.register_view(MonitorStreamView())

    async def mode_changed(event):
        old = event.data.get("old_state")
        new = event.data.get("new_state")
        if old is None or new is None or old.state != "on" or new.state != "off":
            return
        entity = er.async_get(hass).async_get(event.data["entity_id"])
        if (
            entity
            and entity.domain == "switch"
            and entity.original_name in ("Радионяня", "Радионяня: слушатель")
            and entity.device_id
        ):
            await sessions.close_device(entity.device_id)

    # Optimistic ESPHome switch updates also cover a button press before HTTP starts.
    # Registry original_name stays stable when the user renames an entity in HA.
    hass.bus.async_listen(EVENT_STATE_CHANGED, mode_changed)

    async def start_source(call):
        device = call.data["device_id"]
        if sessions.busy(device):
            raise HomeAssistantError("Stop the current monitor pair first")
        name, _ = resolve(device, "source")
        receiver = hass.data[DOMAIN].get(name)
        if receiver is None:
            raise HomeAssistantError(
                "Add this source in Voice PE Monitor integration first"
            )
        await native(device, "source", {"enabled": True})
        try:
            await asyncio.wait_for(receiver.online.wait(), 10)
        except TimeoutError as err:
            await stop_native(device)
            raise HomeAssistantError(
                "Source did not connect: check Mute, key, address and port"
            ) from err

    async def start_listener(call):
        source, listener = call.data["source_device_id"], call.data["device_id"]
        if source == listener:
            raise HomeAssistantError("Source and listener must be different devices")
        if sessions.busy(source) or sessions.busy(listener):
            raise HomeAssistantError("Stop the current monitor pair first")
        name, _ = resolve(source, "source")
        _, service = resolve(listener, "listen")
        receiver = hass.data[DOMAIN].get(name)
        if receiver is None or not receiver.connected:
            raise HomeAssistantError("Run start_source first")
        base = call.data.get("ha_url") or get_url(hass, allow_external=False)
        if not base.startswith(("http://", "https://")):
            raise HomeAssistantError("HA URL must be reachable by the listener")
        token = sessions.create(source, listener, receiver)
        ready_future = sessions.sessions[token]["ready"]
        url = base.rstrip("/") + f"/api/voice_pe_monitor/stream/{token}.wav"
        try:
            await hass.services.async_call(
                "esphome", service, {"url": url}, blocking=True
            )
            ready = await asyncio.wait_for(asyncio.shield(ready_future), 12)
            if not ready:
                raise HomeAssistantError("Listener stopped before connecting")
        except TimeoutError as err:
            await sessions.close(token)
            raise HomeAssistantError(
                "Listener did not connect: check Mute and the HA URL"
            ) from err
        except Exception:
            await sessions.close(token)
            raise

    async def stop(call):
        device = call.data["device_id"]
        if sessions.busy(device):
            await sessions.close_device(device)
        else:
            await stop_native(device)

    device_schema = {vol.Required("device_id"): str}
    hass.services.async_register(
        DOMAIN, "start_source", start_source, schema=vol.Schema(device_schema)
    )
    hass.services.async_register(
        DOMAIN,
        "start_listener",
        start_listener,
        schema=vol.Schema(
            {
                **device_schema,
                vol.Required("source_device_id"): str,
                vol.Optional("ha_url"): str,
            }
        ),
    )
    hass.services.async_register(DOMAIN, "stop", stop, schema=vol.Schema(device_schema))

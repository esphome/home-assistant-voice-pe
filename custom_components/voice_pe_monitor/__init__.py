"""Authenticated HA playback; separate ingest-only device key."""

import base64
from pathlib import Path

import voluptuous as vol
from homeassistant.components import websocket_api
from homeassistant.components.http import StaticPathConfig
from homeassistant.const import EVENT_HOMEASSISTANT_STOP
from homeassistant.exceptions import ConfigEntryNotReady

from .receiver import RATE, Receiver

DOMAIN = "voice_pe_monitor"


async def async_setup(hass, config):
    hass.data.setdefault(DOMAIN, {})
    websocket_api.async_register_command(hass, ws_listen)
    await hass.http.async_register_static_paths(
        [
            StaticPathConfig(
                "/voice_pe_monitor/monitor-card.js",
                str(Path(__file__).parent / "www" / "monitor-card.js"),
                False,
            )
        ]
    )
    return True


async def async_setup_entry(hass, entry):
    receiver = Receiver(entry.data["node_id"], entry.data["upload_key"])
    try:
        await receiver.start("0.0.0.0", entry.data["port"])
    except OSError as err:
        raise ConfigEntryNotReady("Cannot bind audio ingest port") from err
    hass.data[DOMAIN][entry.data["node_id"]] = receiver

    async def stop(event):
        await receiver.stop()

    entry.async_on_unload(hass.bus.async_listen_once(EVENT_HOMEASSISTANT_STOP, stop))
    return True


async def async_unload_entry(hass, entry):
    receiver = hass.data[DOMAIN].pop(entry.data["node_id"])
    await receiver.stop()
    return True


@websocket_api.websocket_command(
    {
        vol.Required("type"): "voice_pe_monitor/listen",
        vol.Required("node_id"): str,
    }
)
@websocket_api.async_response
async def ws_listen(hass, connection, msg):
    # HA's websocket API has already authenticated this connection.
    receiver = hass.data[DOMAIN].get(msg["node_id"])
    if receiver is None:
        connection.send_error(msg["id"], "not_found", "Monitor not configured")
        return
    if len(receiver.listeners) >= 4:
        connection.send_error(msg["id"], "busy", "Maximum four listeners")
        return

    def send(event):
        if event["type"] == "audio":
            event = {
                "type": "audio",
                "pcm": base64.b64encode(event["data"]).decode("ascii"),
                "rate": RATE,
            }
        connection.send_event(msg["id"], event)

    receiver.listeners.add(send)
    connection.subscriptions[msg["id"]] = lambda: receiver.listeners.discard(send)
    connection.send_result(msg["id"])
    send({"type": "status", "connected": receiver.connected})

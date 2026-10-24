"""Deliver per-device ingest credentials through its existing ESPHome connection."""

import logging

from homeassistant.exceptions import HomeAssistantError
from homeassistant.util import slugify

_LOGGER = logging.getLogger(__name__)


async def provision_source(hass, node_id):
    receiver = hass.data.get("voice_pe_monitor", {}).get(node_id)
    service = slugify(f"{node_id}_monitor_configure")
    if receiver is None or not hass.services.has_service("esphome", service):
        return False  # Older firmware may still use its previously configured key.
    try:
        await hass.services.async_call(
            "esphome", service, receiver.provisioning_data(), blocking=True
        )
    except HomeAssistantError:
        # Never log call data, exception payloads or credentials.
        _LOGGER.warning(
            "Could not configure monitor source %s; will retry at startup", node_id
        )
        return False
    return True

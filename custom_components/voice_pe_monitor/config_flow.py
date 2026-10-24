"""One explicitly configured local source per entry/port."""

import re
import secrets

import voluptuous as vol
from homeassistant import config_entries

DOMAIN = "voice_pe_monitor"


class MonitorConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    VERSION = 1

    async def async_step_user(self, user_input=None):
        errors = {}
        if user_input is not None:
            node = user_input["node_id"]
            if not re.fullmatch(r"[a-zA-Z0-9_-]{1,64}", node):
                errors["node_id"] = "invalid_node"
            elif not re.fullmatch(r"[a-zA-Z0-9_-]{32,128}", user_input["upload_key"]):
                errors["upload_key"] = "invalid_key"
            elif any(
                entry.data["port"] == user_input["port"]
                for entry in self._async_current_entries()
            ):
                errors["port"] = "port_in_use"
            else:
                await self.async_set_unique_id(node)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(title=node, data=user_input)
        if not hasattr(self, "_suggested_key"):
            self._suggested_key = secrets.token_urlsafe(32)
        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema(
                {
                    vol.Required("node_id", default="home-assistant-voice-0ab77d"): str,
                    vol.Required("upload_key", default=self._suggested_key): str,
                    vol.Required("port", default=8124): vol.All(
                        vol.Coerce(int), vol.Range(min=1024, max=65535)
                    ),
                }
            ),
            errors=errors,
        )

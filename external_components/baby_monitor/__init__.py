"""Opt-in Voice PE PCM uplink. Not a Home Assistant access token."""

import esphome.codegen as cg
import esphome.config_validation as cv
import voluptuous as vol
from esphome.const import CONF_ID, CONF_MICROPHONE

from esphome.components import microphone, socket

DEPENDENCIES = ["microphone", "network"]
AUTO_LOAD = ["socket"]
ns = cg.esphome_ns.namespace("baby_monitor")
BabyMonitor = ns.class_("BabyMonitor", cg.Component)
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BabyMonitor),
            cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(),
            cv.Required("host"): cv.ipv4address,
            cv.Optional("port", default=8124): cv.port,
            cv.Required("node_id"): vol.Match(r"^[a-zA-Z0-9_-]{1,64}$"),
            cv.Required("upload_key"): cv.All(
                cv.string_strict,
                cv.Length(min=32, max=128),
                vol.Match(r"^[a-zA-Z0-9_-]+$"),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    socket.consume_sockets(1, "baby_monitor"),
)
FINAL_VALIDATE_SCHEMA = cv.Schema(
    {
        cv.Required(
            CONF_MICROPHONE
        ): microphone.final_validate_microphone_source_schema(
            "baby_monitor", sample_rate=16000
        )
    },
    extra=cv.ALLOW_EXTRA,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    source = await microphone.microphone_source_to_code(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(source))
    cg.add(
        var.configure(
            str(config["host"]), config["port"], config["node_id"], config["upload_key"]
        )
    )

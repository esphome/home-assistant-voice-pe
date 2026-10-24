"""Isolated live decoder; composition preserves the final upstream HTTP source."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components.audio_http import media_source as http_source
from esphome.const import CONF_BUFFER_SIZE, CONF_ID

from esphome.components import audio, media_source

from . import ns

AUTO_LOAD = ["audio_http"]
MonitorAudioSource = ns.class_(
    "MonitorAudioSource", cg.Component, media_source.MediaSource
)


def request_decoder(config):
    audio.request_micro_decoder_support()
    audio.request_wav_support()
    return config


CONFIG_SCHEMA = cv.All(
    media_source.media_source_schema(MonitorAudioSource)
    .extend(
        {
            cv.GenerateID("transport_id"): cv.declare_id(
                http_source.AudioHTTPMediaSource
            ),
            cv.Optional(CONF_BUFFER_SIZE, default=8192): cv.int_range(
                min=5000, max=32000
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    request_decoder,
)


async def to_code(config):
    cg.add_define("USE_BABY_MONITOR_AUDIO_SOURCE")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_source.register_media_source(var, config)
    transport = cg.new_Pvariable(config["transport_id"])
    await cg.register_component(transport, config)
    cg.add(transport.set_buffer_size(config[CONF_BUFFER_SIZE]))
    cg.add(transport.set_persistent_ring_buffer(False))
    cg.add(var.set_transport(transport))

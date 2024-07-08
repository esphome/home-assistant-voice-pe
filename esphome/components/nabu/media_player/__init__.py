"""Nabu Media Player Setup."""

import os

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import esp32, media_player, speaker

# from .. import nabu_ns, NabuComponent

CODEOWNERS = ["@synesthesiam", "@kahrendt"]
DEPENDENCIES = ["media_player"]

nabu_ns = cg.esphome_ns.namespace("nabu")
NabuMediaPlayer = nabu_ns.class_("NabuMediaPlayer")
NabuMediaPlayer = nabu_ns.class_(
    "NabuMediaPlayer", NabuMediaPlayer, media_player.MediaPlayer, cg.Component
)

CONF_NABU_ID = "nabu_id"
CONF_SPEAKER = "speaker"

CONFIG_SCHEMA = media_player.MEDIA_PLAYER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(NabuMediaPlayer),
        cv.GenerateID(CONF_SPEAKER): cv.use_id(speaker.Speaker),
    }
)

# @coroutine_with_priority(100.0)
async def to_code(config):
    esp32.add_idf_component(
        name="esp-dsp",
        repo="https://github.com/kahrendt/esp-dsp",
        ref="filename-fix",
        # repo="https://github.com/espressif/esp-dsp",
        # ref="v1.3.0",
    )
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_player.register_media_player(var, config)

    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))

"""Nabu Media Player Setup."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID
from esphome.components import esp32, media_player, speaker

from esphome.components.i2s_audio import (
    BITS_PER_SAMPLE,
    CONF_BITS_PER_SAMPLE,
    CONF_I2S_AUDIO_ID,
    CONF_I2S_DOUT_PIN,
    I2SAudioComponent,
    I2SAudioOut,
    _validate_bits,
)

CODEOWNERS = ["@synesthesiam", "@kahrendt"]
DEPENDENCIES = ["media_player"]

nabu_ns = cg.esphome_ns.namespace("nabu")
NabuMediaPlayer = nabu_ns.class_("NabuMediaPlayer")
NabuMediaPlayer = nabu_ns.class_(
    "NabuMediaPlayer", NabuMediaPlayer, media_player.MediaPlayer, cg.Component, I2SAudioOut,
)

CONFIG_SCHEMA = media_player.MEDIA_PLAYER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(NabuMediaPlayer),
        # cv.GenerateID(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        cv.GenerateID(CONF_I2S_AUDIO_ID): cv.use_id(I2SAudioComponent),
        cv.Required(CONF_I2S_DOUT_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_BITS_PER_SAMPLE, default="16bit"): cv.All(
            _validate_bits, cv.enum(BITS_PER_SAMPLE)
        ),
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
    cg.add_build_flag("-Wno-narrowing") # Necessary to compile helix mp3 decoder

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_player.register_media_player(var, config)
    
    await cg.register_parented(var, config[CONF_I2S_AUDIO_ID])
    cg.add(var.set_dout_pin(config[CONF_I2S_DOUT_PIN]))
    cg.add(var.set_bits_per_sample(config[CONF_BITS_PER_SAMPLE]))

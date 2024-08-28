"""Nabu Media Player Setup."""

import hashlib
import logging
from pathlib import Path
from magic import Magic

import esphome.codegen as cg
import esphome.config_validation as cv

from esphome import automation, external_files, pins
from esphome.components import esp32, i2c, media_player
from esphome.components.media_player import MediaFile, MEDIA_FILE_TYPE_ENUM
from esphome.const import (
    CONF_DURATION,
    CONF_FILE,
    CONF_ID,
    CONF_PATH,
    CONF_RAW_DATA_ID,
    CONF_TYPE,
    CONF_URL,
)
from esphome.core import HexInt, CORE


from esphome.components.i2s_audio import (
    BITS_PER_SAMPLE,
    CONF_BITS_PER_SAMPLE,
    CONF_I2S_AUDIO_ID,
    CONF_I2S_DOUT_PIN,
    I2SAudioComponent,
    I2SAudioOut,
    _validate_bits,
)

_LOGGER = logging.getLogger(__name__)

try:
    from esphome.external_files import download_content
except ImportError:
    from esphome.components.font import download_content

CODEOWNERS = ["@synesthesiam", "@kahrendt"]
DEPENDENCIES = ["media_player"]
DOMAIN = "file"

TYPE_LOCAL = "local"
TYPE_WEB = "web"

CONF_DECIBEL_REDUCTION = "decibel_reduction"

CONF_MEDIA_FILE = "media_file"
CONF_FILES = "files"
CONF_SAMPLE_RATE = "sample_rate"
CONF_VOLUME_INCREMENT = "volume_increment"

nabu_ns = cg.esphome_ns.namespace("nabu")
NabuMediaPlayer = nabu_ns.class_("NabuMediaPlayer")
NabuMediaPlayer = nabu_ns.class_(
    "NabuMediaPlayer",
    NabuMediaPlayer,
    media_player.MediaPlayer,
    cg.Component,
    I2SAudioOut,
    i2c.I2CDevice,
)

DuckingSetAction = nabu_ns.class_(
    "DuckingSetAction", automation.Action, cg.Parented.template(NabuMediaPlayer)
)
PlayLocalMediaAction = nabu_ns.class_(
    "PlayLocalMediaAction", automation.Action, cg.Parented.template(NabuMediaPlayer)
)


def _compute_local_file_path(value: dict) -> Path:
    url = value[CONF_URL]
    h = hashlib.new("sha256")
    h.update(url.encode())
    key = h.hexdigest()[:8]
    base_dir = external_files.compute_local_file_dir(DOMAIN)
    _LOGGER.debug("_compute_local_file_path: base_dir=%s", base_dir / key)
    return base_dir / key


def _download_web_file(value):
    url = value[CONF_URL]
    path = _compute_local_file_path(value)

    download_content(url, path)
    _LOGGER.debug("download_web_file: path=%s", path)
    return value


LOCAL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_PATH): cv.file_,
    }
)

WEB_SCHEMA = cv.All(
    {
        cv.Required(CONF_URL): cv.url,
    },
    _download_web_file,
)


def _validate_file_shorthand(value):
    value = cv.string_strict(value)
    if value.startswith("http://") or value.startswith("https://"):
        return _file_schema(
            {
                CONF_TYPE: TYPE_WEB,
                CONF_URL: value,
            }
        )
    return _file_schema(
        {
            CONF_TYPE: TYPE_LOCAL,
            CONF_PATH: value,
        }
    )


TYPED_FILE_SCHEMA = cv.typed_schema(
    {
        TYPE_LOCAL: LOCAL_SCHEMA,
        TYPE_WEB: WEB_SCHEMA,
    },
)


def _file_schema(value):
    if isinstance(value, str):
        return _validate_file_shorthand(value)
    return TYPED_FILE_SCHEMA(value)


def _validate_file_shorthand(value):
    value = cv.string_strict(value)
    if value.startswith("http://") or value.startswith("https://"):
        return _file_schema(
            {
                CONF_TYPE: TYPE_WEB,
                CONF_URL: value,
            }
        )
    return _file_schema(
        {
            CONF_TYPE: TYPE_LOCAL,
            CONF_PATH: value,
        }
    )


TYPED_FILE_SCHEMA = cv.typed_schema(
    {
        TYPE_LOCAL: LOCAL_SCHEMA,
        TYPE_WEB: WEB_SCHEMA,
    },
)


def _file_schema(value):
    if isinstance(value, str):
        return _validate_file_shorthand(value)
    return TYPED_FILE_SCHEMA(value)


MEDIA_FILE_TYPE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ID): cv.declare_id(MediaFile),
        cv.Required(CONF_FILE): _file_schema,
        cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
    }
)


CONFIG_SCHEMA = media_player.MEDIA_PLAYER_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(NabuMediaPlayer),
        cv.GenerateID(CONF_I2S_AUDIO_ID): cv.use_id(I2SAudioComponent),
        cv.Required(CONF_I2S_DOUT_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=1),
        cv.Optional(CONF_BITS_PER_SAMPLE, default="16bit"): cv.All(
            _validate_bits, cv.enum(BITS_PER_SAMPLE)
        ),
        cv.Optional(CONF_VOLUME_INCREMENT, default=0.05): cv.percentage,
        cv.Optional(CONF_FILES): cv.ensure_list(MEDIA_FILE_TYPE_SCHEMA),
    }
).extend(i2c.i2c_device_schema(0x18))


def _read_audio_file_and_type(file_config):
    conf_file = file_config[CONF_FILE]
    file_source = conf_file[CONF_TYPE]
    if file_source == TYPE_LOCAL:
        path = CORE.relative_config_path(conf_file[CONF_PATH])
    elif file_source == TYPE_WEB:
        path = _compute_local_file_path(conf_file)

    with open(path, "rb") as f:
        data = f.read()

    magic = Magic(mime=True)
    file_type = magic.from_buffer(data)

    media_file_type = MEDIA_FILE_TYPE_ENUM["NONE"]
    if "wav" in file_type:
        media_file_type = MEDIA_FILE_TYPE_ENUM["WAV"]
    elif "mpeg" in file_type:
        media_file_type = MEDIA_FILE_TYPE_ENUM["MP3"]
    elif "flac" in file_type:
        media_file_type = MEDIA_FILE_TYPE_ENUM["FLAC"]

    return data, media_file_type


def _supported_local_file_validate(config):
    if files_list := config.get(CONF_FILES):
        for file_config in files_list:
            _, media_file_type = _read_audio_file_and_type(file_config)
            if str(media_file_type) == str(MEDIA_FILE_TYPE_ENUM["NONE"]):
                raise cv.Invalid("Unsupported local media file.")


FINAL_VALIDATE_SCHEMA = _supported_local_file_validate


async def to_code(config):
    esp32.add_idf_component(
        name="esp-dsp",
        repo="https://github.com/kahrendt/esp-dsp",
        ref="no-round-dot-product",
    )

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await media_player.register_media_player(var, config)
    await i2c.register_i2c_device(var, config)

    await cg.register_parented(var, config[CONF_I2S_AUDIO_ID])
    cg.add(var.set_dout_pin(config[CONF_I2S_DOUT_PIN]))
    cg.add(var.set_bits_per_sample(config[CONF_BITS_PER_SAMPLE]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))

    cg.add(var.set_volume_increment(config[CONF_VOLUME_INCREMENT]))

    if files_list := config.get(CONF_FILES):
        for file_config in files_list:
            data, media_file_type = _read_audio_file_and_type(file_config)

            rhs = [HexInt(x) for x in data]
            prog_arr = cg.progmem_array(file_config[CONF_RAW_DATA_ID], rhs)

            media_files_struct = cg.StructInitializer(
                MediaFile,
                (
                    "data",
                    prog_arr,
                ),
                (
                    "length",
                    len(rhs),
                ),
                (
                    "file_type",
                    media_file_type,
                ),
            )

            cg.new_Pvariable(
                file_config[CONF_ID],
                media_files_struct,
            )
            # decl = VariableDeclarationExpression(type, "*", name)
            # CORE.add_global(decl)
            # var = MockObj(name, "->")
            # CORE.register_variable(name, var)
            # return var

            # CORE.register_variable(MediaFile, file_config[CONF_ID])


DUCKING_SET_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(NabuMediaPlayer),
        cv.Required(CONF_DECIBEL_REDUCTION): cv.templatable(
            cv.int_range(min=0, max=51)
        ),
        cv.Optional(CONF_DURATION, default="0.0s"): cv.templatable(
            cv.positive_time_period_seconds
        ),
    }
)


# @automation.register_action(
#     "nabu.play_local_media_file",
#     PlayLocalMediaAction,
#     cv.maybe_simple_value(
#         {
#             cv.GenerateID(): cv.use_id(NabuMediaPlayer),
#             cv.Required(CONF_MEDIA_FILE): cv.use_id(MediaFile),
#         },
#         key=CONF_MEDIA_FILE,
#     ),
# )
# async def media_player_play_media_action(config, action_id, template_arg, args):
#     var = cg.new_Pvariable(action_id, template_arg)
#     await cg.register_parented(var, config[CONF_ID])
#     media_file = config[CONF_MEDIA_FILE]
#     cg.add(var.set_media_file(media_file))
#     return var


@automation.register_action("nabu.set_ducking", DuckingSetAction, DUCKING_SET_SCHEMA)
async def ducking_set_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    decibel_reduction = await cg.templatable(
        config[CONF_DECIBEL_REDUCTION], args, cg.uint8
    )
    cg.add(var.set_decibel_reduction(decibel_reduction))
    duration = await cg.templatable(config[CONF_DURATION], args, cg.float_)
    cg.add(var.set_duration(duration))
    return var

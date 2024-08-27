import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.automation import maybe_simple_id
from esphome.core import coroutine_with_priority
from esphome.const import CONF_ID, CONF_VOLUME

CODEOWNERS = ["@kbx81"]
IS_PLATFORM_COMPONENT = True

audio_dac_ns = cg.esphome_ns.namespace("audio_dac")
AudioDac = audio_dac_ns.class_("AudioDac", cg.Component)

MuteOffAction = audio_dac_ns.class_("MuteOffAction", automation.Action)
MuteOnAction = audio_dac_ns.class_("MuteOnAction", automation.Action)
SetVolumeAction = audio_dac_ns.class_("SetVolumeAction", automation.Action)


CONFIG_SCHEMA_BASE = cv.COMPONENT_SCHEMA


MUTE_ACTION_SCHEMA = maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(AudioDac),
    }
)

SET_VOLUME_ACTION_SCHEMA = cv.maybe_simple_value(
    {
        cv.GenerateID(): cv.use_id(AudioDac),
        cv.Required(CONF_VOLUME): cv.templatable(
            cv.All(cv.decibel, cv.float_range(max=24, min=-63.5))
        ),
    },
    key=CONF_VOLUME,
)


@automation.register_action("audio_dac.mute_off", MuteOffAction, MUTE_ACTION_SCHEMA)
@automation.register_action("audio_dac.mute_on", MuteOnAction, MUTE_ACTION_SCHEMA)
async def audio_dac_mute_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "audio_dac.set_volume", SetVolumeAction, SET_VOLUME_ACTION_SCHEMA
)
async def audio_dac_set_volume_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)

    template_ = await cg.templatable(config.get(CONF_VOLUME), args, float)
    cg.add(var.set_volume(template_))

    return var


@coroutine_with_priority(100.0)
async def to_code_base(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add_define("USE_AUDIO_DAC")
    cg.add_global(audio_dac_ns.using)

    return var

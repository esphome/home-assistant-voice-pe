import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome import automation
from esphome.automation import maybe_simple_id
from esphome.const import CONF_ID, CONF_MODE, CONF_VOLUME

CODEOWNERS = ["@kbx81"]
DEPENDENCIES = ["i2c"]

aic3204_ns = cg.esphome_ns.namespace("aic3204")
AIC3204 = aic3204_ns.class_("AIC3204", cg.Component, i2c.I2CDevice)

MuteOffAction = aic3204_ns.class_("MuteOffAction", automation.Action)
MuteOnAction = aic3204_ns.class_("MuteOnAction", automation.Action)
SetAutoMuteAction = aic3204_ns.class_("SetAutoMuteAction", automation.Action)
SetVolumeAction = aic3204_ns.class_("SetVolumeAction", automation.Action)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AIC3204),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x18))
)


MUTE_ACTION_SCHEMA = maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(AIC3204),
    }
)

SET_AUTO_MUTE_ACTION_SCHEMA = cv.maybe_simple_value(
    {
        cv.GenerateID(): cv.use_id(AIC3204),
        cv.Required(CONF_MODE): cv.templatable(cv.int_range(max=7, min=0)),
    },
    key=CONF_MODE,
)

SET_VOLUME_ACTION_SCHEMA = cv.maybe_simple_value(
    {
        cv.GenerateID(): cv.use_id(AIC3204),
        cv.Required(CONF_VOLUME): cv.templatable(
            cv.All(cv.decibel, cv.float_range(max=24, min=-63.5))
        ),
    },
    key=CONF_VOLUME,
)


@automation.register_action("aic3204.mute_off", MuteOffAction, MUTE_ACTION_SCHEMA)
@automation.register_action("aic3204.mute_on", MuteOnAction, MUTE_ACTION_SCHEMA)
async def aic3204_mute_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "aic3204.set_auto_mute_mode", SetAutoMuteAction, SET_AUTO_MUTE_ACTION_SCHEMA
)
async def aic3204_set_volume_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)

    template_ = await cg.templatable(config.get(CONF_MODE), args, int)
    cg.add(var.set_auto_mute_mode(template_))

    return var


@automation.register_action(
    "aic3204.set_volume", SetVolumeAction, SET_VOLUME_ACTION_SCHEMA
)
async def aic3204_set_volume_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, paren)

    template_ = await cg.templatable(config.get(CONF_VOLUME), args, float)
    cg.add(var.set_volume(template_))

    return var


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

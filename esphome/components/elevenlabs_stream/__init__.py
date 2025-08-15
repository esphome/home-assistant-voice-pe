import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, core
from esphome.automation import Condition
from esphome.components import speaker, microphone
from esphome.const import (
    CONF_ID,
    CONF_ON_ERROR,
    CONF_TRIGGER_ID,
    CONF_URL,
)

CODEOWNERS = ["@ffMathy"]
DEPENDENCIES = ["network", "wifi", "json"]

CONF_AGENT_ID = "agent_id"
CONF_API_KEY = "api_key"
CONF_ON_START = "on_start"
CONF_ON_END = "on_end"
CONF_MICROPHONE = "microphone"
CONF_ELEVENLABS_SPEAKER = "elevenlabs_speaker"
CONF_ACTIVATION_SPEAKER = "activation_speaker"

elevenlabs_stream_ns = cg.esphome_ns.namespace("elevenlabs_stream")
ElevenLabsStream = elevenlabs_stream_ns.class_("ElevenLabsStream", cg.Component)
ElevenLabsStreamIsRunningCondition = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamIsRunningCondition", Condition
)

# Actions
ElevenLabsStreamStartAction = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamStartAction", automation.Action
)
ElevenLabsStreamStopAction = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamStopAction", automation.Action
)

# Triggers - simplified
ElevenLabsStreamStartTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamStartTrigger", automation.Trigger.template()
)
ElevenLabsStreamEndTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamEndTrigger", automation.Trigger.template()
)
ElevenLabsStreamErrorTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamErrorTrigger", automation.Trigger.template(cg.std_string)
)
# New triggers for voice assistant states
ElevenLabsStreamListeningTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamListeningTrigger", automation.Trigger.template()
)
ElevenLabsStreamProcessingTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamProcessingTrigger", automation.Trigger.template()
)
ElevenLabsStreamReplyingTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamReplyingTrigger", automation.Trigger.template()
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ElevenLabsStream),
        cv.Required(CONF_AGENT_ID): cv.templatable(cv.string),
        cv.Optional(CONF_API_KEY): cv.templatable(cv.string),
        cv.Optional(CONF_MICROPHONE): cv.use_id(cg.Parented),
        cv.Optional(CONF_ELEVENLABS_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_ACTIVATION_SPEAKER): cv.use_id(speaker.Speaker),
        cv.Optional(CONF_ON_START): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamStartTrigger),
            }
        ),
        cv.Optional(CONF_ON_END): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamEndTrigger),
            }
        ),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamErrorTrigger),
            }
        ),
        cv.Optional("on_listening"): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamListeningTrigger),
            }
        ),
        cv.Optional("on_processing"): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamProcessingTrigger),
            }
        ),
        cv.Optional("on_replying"): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamReplyingTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Set agent ID
    template_ = await cg.templatable(config[CONF_AGENT_ID], [], cg.std_string)
    cg.add(var.set_agent_id(template_))

    # Set API key if provided (mark as secret)
    if CONF_API_KEY in config:
        template_ = await cg.templatable(config[CONF_API_KEY], [], cg.std_string)
        cg.add(var.set_api_key(template_))

    # Set microphone (if provided)
    if CONF_MICROPHONE in config:
        mic = await cg.get_variable(config[CONF_MICROPHONE])
        cg.add(var.set_microphone(mic))

    # Set ElevenLabs speaker (if provided)
    if CONF_ELEVENLABS_SPEAKER in config:
        elevenlabs_speaker = await cg.get_variable(config[CONF_ELEVENLABS_SPEAKER])
        cg.add(var.set_elevenlabs_speaker(elevenlabs_speaker))

    # Set activation speaker (if provided)
    if CONF_ACTIVATION_SPEAKER in config:
        activation_speaker = await cg.get_variable(config[CONF_ACTIVATION_SPEAKER])
        cg.add(var.set_activation_speaker(activation_speaker))

    # Register triggers
    for conf in config.get(CONF_ON_START, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_start_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_END, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_end_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_error_trigger(trigger))
        await automation.build_automation(trigger, [(cg.std_string, "error_message")], conf)

    # Register new triggers
    for conf in config.get("on_listening", []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_listening_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get("on_processing", []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_processing_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get("on_replying", []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_replying_trigger(trigger))
        await automation.build_automation(trigger, [], conf)


# Actions
@automation.register_action(
    "elevenlabs_stream.start",
    ElevenLabsStreamStartAction,
    cv.Schema({cv.GenerateID(): cv.use_id(ElevenLabsStream)}),
)
async def elevenlabs_stream_start_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_action(
    "elevenlabs_stream.stop",
    ElevenLabsStreamStopAction,
    cv.Schema({cv.GenerateID(): cv.use_id(ElevenLabsStream)}),
)
async def elevenlabs_stream_stop_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition(
    "elevenlabs_stream.is_running",
    automation.LambdaCondition,
    cv.Schema({cv.GenerateID(): cv.use_id(ElevenLabsStream)}),
)
async def elevenlabs_stream_is_running_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    # Accept & ignore any Ts... from the surrounding trigger
    lambda_code = f"[=](auto&&...) -> bool {{ return {parent}->is_running(); }}"
    return cg.new_Pvariable(condition_id, template_arg, cg.RawExpression(lambda_code))
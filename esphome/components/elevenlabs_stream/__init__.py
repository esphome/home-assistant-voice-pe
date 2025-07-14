import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, core
from esphome.automation import Condition
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
CONF_ON_LISTENING = "on_listening"
CONF_ON_SPEAKING = "on_speaking"
CONF_ON_CONNECTED = "on_connected"
CONF_ON_DISCONNECTED = "on_disconnected"
CONF_MICROPHONE = "microphone"
CONF_SPEAKER = "speaker"

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

# Triggers
ElevenLabsStreamStartTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamStartTrigger", automation.Trigger.template()
)
ElevenLabsStreamEndTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamEndTrigger", automation.Trigger.template()
)
ElevenLabsStreamListeningTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamListeningTrigger", automation.Trigger.template()
)
ElevenLabsStreamSpeakingTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamSpeakingTrigger", automation.Trigger.template()
)
ElevenLabsStreamConnectedTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamConnectedTrigger", automation.Trigger.template()
)
ElevenLabsStreamDisconnectedTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamDisconnectedTrigger", automation.Trigger.template()
)
ElevenLabsStreamErrorTrigger = elevenlabs_stream_ns.class_(
    "ElevenLabsStreamErrorTrigger", automation.Trigger.template(cg.std_string)
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ElevenLabsStream),
        cv.Required(CONF_AGENT_ID): cv.templatable(cv.string),
        cv.Optional(CONF_API_KEY): cv.templatable(cv.string),
        cv.Optional(CONF_MICROPHONE): cv.use_id(cg.Parented),  # Made optional for testing
        cv.Optional(CONF_SPEAKER): cv.use_id(cg.Parented),    # Made optional for testing
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
        cv.Optional(CONF_ON_LISTENING): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamListeningTrigger),
            }
        ),
        cv.Optional(CONF_ON_SPEAKING): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamSpeakingTrigger),
            }
        ),
        cv.Optional(CONF_ON_CONNECTED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamConnectedTrigger),
            }
        ),
        cv.Optional(CONF_ON_DISCONNECTED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamDisconnectedTrigger),
            }
        ),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ElevenLabsStreamErrorTrigger),
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

    # Set API key if provided
    if CONF_API_KEY in config:
        template_ = await cg.templatable(config[CONF_API_KEY], [], cg.std_string)
        cg.add(var.set_api_key(template_))

    # Set microphone (if provided)
    if CONF_MICROPHONE in config:
        mic = await cg.get_variable(config[CONF_MICROPHONE])
        cg.add(var.set_microphone(mic))

    # Set speaker (if provided)
    if CONF_SPEAKER in config:
        speaker = await cg.get_variable(config[CONF_SPEAKER])
        cg.add(var.set_speaker(speaker))

    # Register triggers
    for conf in config.get(CONF_ON_START, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_start_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_END, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_end_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_LISTENING, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_listening_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_SPEAKING, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_speaking_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_CONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_connected_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_DISCONNECTED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_disconnected_trigger(trigger))
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
        cg.add(var.add_on_error_trigger(trigger))
        await automation.build_automation(trigger, [(cg.std_string, "error_message")], conf)


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


# Conditions
@automation.register_condition(
    "elevenlabs_stream.is_running",
    automation.LambdaCondition,
    cv.Schema({cv.GenerateID(): cv.use_id(ElevenLabsStream)}),
)
async def elevenlabs_stream_is_running_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    # Create a lambda that returns the connection status
    lambda_code = f"[=]() {{ return {parent}->is_connected(); }}"
    return cg.new_Pvariable(condition_id, template_arg, cg.RawExpression(lambda_code))

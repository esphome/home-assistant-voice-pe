import esphome.config_validation as cv
import esphome.codegen as cg

from esphome import pins
from esphome.const import CONF_ID, CONF_NUMBER
from esphome.components import microphone, esp32
from esphome.components.adc import ESP32_VARIANT_ADC1_PIN_TO_CHANNEL, validate_adc_pin

from esphome.components.i2s_audio import (
    I2SAudioComponent,
    I2SAudioIn,
    CONF_I2S_MODE,
    CONF_PRIMARY,
    I2S_MODE_OPTIONS,
    I2S_BITS_PER_SAMPLE,
    CONF_BITS_PER_SAMPLE,
    CONF_I2S_AUDIO_ID,
    CONF_I2S_DIN_PIN,
    _validate_bits,
)

CODEOWNERS = ["@kahrendt"]
DEPENDENCIES = ["i2s_audio"]

CONF_ADC_PIN = "adc_pin"
CONF_ADC_TYPE = "adc_type"
CONF_PDM = "pdm"
CONF_SAMPLE_RATE = "sample_rate"
CONF_USE_APLL = "use_apll"
CONF_CHANNEL_0 = "channel_0"
CONF_CHANNEL_1 = "channel_1"
CONF_AMPLIFY_SHIFT = "amplify_shift"

nabu_microphone_ns = cg.esphome_ns.namespace("nabu_microphone")

NabuMicrophone = nabu_microphone_ns.class_("NabuMicrophone", I2SAudioIn, cg.Component)
NabuMicrophoneChannel = nabu_microphone_ns.class_(
    "NabuMicrophoneChannel", microphone.Microphone, cg.Component
)

i2s_channel_fmt_t = cg.global_ns.enum("i2s_channel_fmt_t")
CHANNELS = {
    "left": i2s_channel_fmt_t.I2S_CHANNEL_FMT_ONLY_LEFT,
    "right": i2s_channel_fmt_t.I2S_CHANNEL_FMT_ONLY_RIGHT,
}

INTERNAL_ADC_VARIANTS = [esp32.const.VARIANT_ESP32]
PDM_VARIANTS = [esp32.const.VARIANT_ESP32, esp32.const.VARIANT_ESP32S3]


def validate_esp32_variant(config):
    variant = esp32.get_esp32_variant()
    if config[CONF_ADC_TYPE] == "external":
        if config[CONF_PDM]:
            if variant not in PDM_VARIANTS:
                raise cv.Invalid(f"{variant} does not support PDM")
        return config
    if config[CONF_ADC_TYPE] == "internal":
        if variant not in INTERNAL_ADC_VARIANTS:
            raise cv.Invalid(f"{variant} does not have an internal ADC")
        return config
    raise NotImplementedError


MICROPHONE_CHANNEL_SCHEMA = microphone.MICROPHONE_SCHEMA.extend(
            {
                cv.GenerateID(): cv.declare_id(NabuMicrophoneChannel),
                cv.Optional(CONF_AMPLIFY_SHIFT, default=0): cv.All(
                    cv.uint8_t, cv.Range(min=0, max=8)
                ),
            }
        )

BASE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(NabuMicrophone),
        cv.GenerateID(CONF_I2S_AUDIO_ID): cv.use_id(I2SAudioComponent),
        cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.int_range(min=1),
        cv.Optional(CONF_BITS_PER_SAMPLE, default="32bit"): cv.All(
            _validate_bits, cv.enum(I2S_BITS_PER_SAMPLE)
        ),
        cv.Optional(CONF_I2S_MODE, default=CONF_PRIMARY): cv.enum(
            I2S_MODE_OPTIONS, lower=True
        ),
        cv.Optional(CONF_USE_APLL, default=False): cv.boolean,
        cv.Optional(CONF_CHANNEL_0): MICROPHONE_CHANNEL_SCHEMA,
        cv.Optional(CONF_CHANNEL_1): MICROPHONE_CHANNEL_SCHEMA,
    }
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.All(
    cv.typed_schema(
        {
            "internal": BASE_SCHEMA.extend(
                {
                    cv.Required(CONF_ADC_PIN): validate_adc_pin,
                }
            ),
            "external": BASE_SCHEMA.extend(
                {
                    cv.Required(CONF_I2S_DIN_PIN): pins.internal_gpio_input_pin_number,
                    cv.Required(CONF_PDM): cv.boolean,
                }
            ),
        },
        key=CONF_ADC_TYPE,
    ),
    validate_esp32_variant,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    await cg.register_parented(var, config[CONF_I2S_AUDIO_ID])

    if channel_0_config := config.get(CONF_CHANNEL_0):
        channel_0 = cg.new_Pvariable(channel_0_config[CONF_ID])
        await cg.register_component(channel_0, channel_0_config)
        await cg.register_parented(channel_0, config[CONF_ID])
        await microphone.register_microphone(channel_0, channel_0_config)
        cg.add(var.set_channel_0(channel_0))
        cg.add(channel_0.set_amplify_shift(channel_0_config[CONF_AMPLIFY_SHIFT]))

    if channel_1_config := config.get(CONF_CHANNEL_1):
        channel_1 = cg.new_Pvariable(channel_1_config[CONF_ID])
        await cg.register_component(channel_1, channel_1_config)
        await cg.register_parented(channel_1, config[CONF_ID])
        await microphone.register_microphone(channel_1, channel_1_config)
        cg.add(var.set_channel_1(channel_1))
        cg.add(channel_1.set_amplify_shift(channel_1_config[CONF_AMPLIFY_SHIFT]))

    if config[CONF_ADC_TYPE] == "internal":
        variant = esp32.get_esp32_variant()
        pin_num = config[CONF_ADC_PIN][CONF_NUMBER]
        channel = ESP32_VARIANT_ADC1_PIN_TO_CHANNEL[variant][pin_num]
        cg.add(var.set_adc_channel(channel))
    else:
        cg.add(var.set_din_pin(config[CONF_I2S_DIN_PIN]))
        cg.add(var.set_pdm(config[CONF_PDM]))

    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_bits_per_sample(config[CONF_BITS_PER_SAMPLE]))
    cg.add(var.set_use_apll(config[CONF_USE_APLL]))
    cg.add(var.set_i2s_mode(config[CONF_I2S_MODE]))

    cg.add_define("USE_OTA_STATE_CALLBACK")

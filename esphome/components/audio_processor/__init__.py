import esphome.codegen as cg
from esphome.core import coroutine_with_priority

CODEOWNERS = ["@kbx81"]
IS_PLATFORM_COMPONENT = True

audio_processor_ns = cg.esphome_ns.namespace("audio_processor")


@coroutine_with_priority(100.0)
async def to_code(config):
    cg.add_define("USE_AUDIO_PROCESSOR")
    # cg.add_global(audio_processor_ns.using)

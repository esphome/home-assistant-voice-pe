// json.cpp
#include "json.h"
#include <esp_heap_caps.h>
#include <ArduinoJson.h>

namespace esphome {
namespace elevenlabs_stream {

struct PSRAMAllocator {
    void *allocate(size_t size) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void *pointer) {
        heap_caps_free(pointer);
    }
    void *reallocate(void *ptr, size_t new_size) {
        return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

JsonObject JsonDeserializer::parse(const uint8_t* buffer, size_t length, std::string& error_out) {
    BasicJsonDocument<PSRAMAllocator> json_document(length + 1024); // Extra space for parsing overhead
    if (json_document.overflowed()) {
        error_out = "Could not allocate memory for JSON document!";
        return JsonObject();
    }
    DeserializationError err = deserializeJson(json_document, (const char*)buffer, length);
    if (err != DeserializationError::Ok) {
        error_out = err.c_str();
        return JsonObject();
    }
    return json_document.as<JsonObject>();
}

} // namespace elevenlabs_stream
} // namespace esphome


// json.cpp
#include "json.h"
#include <esp_heap_caps.h>
#include "esphome/core/log.h"
#include <esphome/components/json/json_util.h>

namespace esphome {
namespace elevenlabs_stream {

static const char *TAG = "json";

std::string JsonDeserializer::to_string(const JsonObject& obj) {
  std::string out;
  serializeJson(obj, out);
  return out;
}

std::unique_ptr<BasicJsonDocument<PSRAMAllocator>> JsonDeserializer::parse(const uint8_t* buffer, size_t length) {
    auto json_document = std::make_unique<BasicJsonDocument<PSRAMAllocator>>(length + 1024 * 10); // Extra space for parsing overhead
    if (json_document->overflowed()) {
        ESP_LOGD(TAG, "parse: JSON document overflowed");
        return nullptr;
    }
    DeserializationError err = deserializeJson(*json_document, (const char*)buffer, length);
    if (err != DeserializationError::Ok) {
        ESP_LOGD(TAG, "parse: JSON deserialization failed: %s", err.c_str());
        return nullptr;
    }
    return json_document;
}

std::unique_ptr<BasicJsonDocument<PSRAMAllocator>> JsonDeserializer::parse(const char* cstr) {
    return parse(reinterpret_cast<const uint8_t*>(cstr), strlen(cstr));
}

} // namespace elevenlabs_stream
} // namespace esphome

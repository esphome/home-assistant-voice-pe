// json.h
#pragma once
#include <ArduinoJson.h>
#include <cstddef>
#include <string>

namespace esphome {
namespace elevenlabs_stream {

class JsonDeserializer {
public:
    // Parses a JSON buffer and returns a JsonObject. Returns nullptr on error.
    static JsonObject parse(const uint8_t* buffer, size_t length, std::string& error_out);
};

} // namespace elevenlabs_stream
} // namespace esphome

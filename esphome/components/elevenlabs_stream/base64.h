#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace esphome {
namespace elevenlabs_stream {



std::string base64_encode(const uint8_t* data, size_t len);
// Returns nullptr on failure, otherwise buffer must be freed by caller with heap_caps_free
uint8_t* base64_decode(const char* base64_data, size_t& out_len);

} // namespace elevenlabs_stream
} // namespace esphome

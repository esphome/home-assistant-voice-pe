#include "base64.h"
#include <mbedtls/base64.h>
#include "esphome/core/log.h"
#include <string>
#include <cstring>
#include <memory>
#include <esp_heap_caps.h>

namespace esphome {
namespace elevenlabs_stream {

static const char* TAG = "base64";

// Base64 encoding function with error handling
std::string base64_encode(const uint8_t* data, size_t len) {
  if (!data || len == 0) {
    return "";
  }
  size_t output_len = 0;
  int ret = mbedtls_base64_encode(nullptr, 0, &output_len, data, len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    ESP_LOGE(TAG, "Failed to calculate base64 encode buffer size: %d", ret);
    return "";
  }
  std::string result(output_len, '\0');
  ret = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&result[0]), output_len, &output_len, data, len);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to encode base64: %d", ret);
    return "";
  }
  result.resize(output_len);
  return result;
}

// Base64 decoding function with error handling, using unique_ptr for large buffer efficiency

// Returns nullptr on failure, otherwise buffer must be freed by caller with heap_caps_free
uint8_t* base64_decode(const char* base64_data, size_t& out_len) {
  out_len = 0;
  if (!base64_data) {
    ESP_LOGE(TAG, "base64_decode: input is null");
    return nullptr;
  }
  size_t input_len = strlen(base64_data);
  if (input_len == 0) {
    return nullptr;
  }
  size_t required_output_len = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &required_output_len, (const unsigned char*)base64_data, input_len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    ESP_LOGE(TAG, "base64_decode: Failed to get output length: %d", ret);
    return nullptr;
  }
  uint8_t* buffer = (uint8_t*) heap_caps_malloc(required_output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    ESP_LOGE(TAG, "base64_decode: Failed to allocate %zu bytes in PSRAM", required_output_len);
    return nullptr;
  }
  size_t output_len = 0;
  ret = mbedtls_base64_decode(buffer, required_output_len, &output_len, (const unsigned char*)base64_data, input_len);
  if (ret != 0) {
    ESP_LOGE(TAG, "base64_decode: Failed to decode: %d", ret);
    heap_caps_free(buffer);
    return nullptr;
  }
  out_len = output_len;
  return buffer;
}

} // namespace elevenlabs_stream
} // namespace esphome

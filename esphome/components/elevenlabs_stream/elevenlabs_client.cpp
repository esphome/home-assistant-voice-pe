// elevenlabs_client.cpp
// Implements ElevenLabsClient: handles signed URL, HTTP, WebSocket connection, and protocol details.

#include "elevenlabs_client.h"
#include "esphome/core/log.h"
#include "json.h"
#include "http_client.h"
#include <string>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>

namespace esphome {
namespace elevenlabs_stream {


static const char *TAG = "elevenlabs_client";
static const char *const ELEVENLABS_HOST = "api.elevenlabs.io";
static const int ELEVENLABS_PORT = 443;
static const char *const ELEVENLABS_SIGNED_URL_PATH = "/v1/convai/conversation/get_signed_url";

// WebsocketClient implementation
#include "websocket_client.h"

ElevenLabsClient::ElevenLabsClient(const std::string &agent_id, const std::string &api_key)
    : agent_id_(agent_id), api_key_(api_key)
{
    ESP_LOGI(TAG, "=== CONSTRUCTOR CALLED ===");
    websocket_ = std::make_unique<WebsocketClient>();
}

ElevenLabsClient::~ElevenLabsClient() { disconnect(); }

bool ElevenLabsClient::get_signed_url(std::string &signed_url_out) {
  ESP_LOGI(TAG, "=== GET_SIGNED_URL START ===");
  ESP_LOGI(TAG, "GET_SIGNED_URL: Getting signed URL from ElevenLabs...");
  ESP_LOGD(TAG, "GET_SIGNED_URL: Agent ID='%s'", this->agent_id_.c_str());
  ESP_LOGD(TAG, "GET_SIGNED_URL: API Key configured=%s", this->api_key_.empty() ? "NO" : "YES");

  if (this->agent_id_.empty()) {
    ESP_LOGE(TAG, "GET_SIGNED_URL: Agent ID not configured");
    return false;
  }

  // Create the URL with agent_id as query parameter
  std::string url = "https://";
  url += ELEVENLABS_HOST;
  url += ELEVENLABS_SIGNED_URL_PATH;
  url += "?agent_id=" + this->agent_id_;

  ESP_LOGD(TAG, "GET_SIGNED_URL: Request URL: %s", url.c_str());

  // Clear any previous signed URL
  ESP_LOGD(TAG, "GET_SIGNED_URL: Cleared previous signed URL");

  std::map<std::string, std::string> headers;
  if (!this->api_key_.empty()) {
    headers["xi-api-key"] = this->api_key_;
  }
  std::string response;
  bool http_ok = HttpClient::get(url, headers, response);
  if (!http_ok) {
    ESP_LOGE(TAG, "GET_SIGNED_URL: HTTP request failed");
    ESP_LOGE(TAG, "=== GET_SIGNED_URL FAILED ===");
    return false;
  }
  if (!response.empty()) {
    ESP_LOGD(TAG, "GET_SIGNED_URL: Parsing JSON response...");
    ESP_LOGD(TAG, "GET_SIGNED_URL: Full response: %s", response.c_str());
    auto json_doc = JsonDeserializer::parse(response.c_str());
    if (!json_doc) {
      ESP_LOGE(TAG, "GET_SIGNED_URL: Failed to parse JSON response");
      ESP_LOGE(TAG, "=== GET_SIGNED_URL FAILED ===");
      return false;
    }
    JsonObject root = json_doc->as<JsonObject>();
    ESP_LOGD(TAG, "GET_SIGNED_URL: root.isNull() = %d", root.isNull());
    ESP_LOGD(TAG, "GET_SIGNED_URL: root.size() = %d", root.size());
    ESP_LOGD(TAG, "GET_SIGNED_URL: Available fields and values in root:");
    for (JsonPair kv : root) {
      ESP_LOGD(TAG, "GET_SIGNED_URL:   - %s: %s", kv.key().c_str(), kv.value().as<const char*>() ? kv.value().as<const char*>() : "<non-string>");
    }
    if (root.containsKey("signed_url")) {
      const char *signed_url = root["signed_url"];
      if (signed_url) {
        signed_url_out = std::string(signed_url);
        ESP_LOGI(TAG, "GET_SIGNED_URL: Extracted signed URL: %s", signed_url_out.c_str());
        ESP_LOGI(TAG, "GET_SIGNED_URL: Got signed URL successfully");
        ESP_LOGD(TAG, "=== GET_SIGNED_URL SUCCESS ===");
        return true;
      } else {
        ESP_LOGE(TAG, "GET_SIGNED_URL: signed_url key exists but value is null");
      }
    } else {
      ESP_LOGE(TAG, "GET_SIGNED_URL: signed_url field not found in response");
    }
    ESP_LOGE(TAG, "=== GET_SIGNED_URL FAILED ===");
    return false;
  } else {
    ESP_LOGE(TAG, "GET_SIGNED_URL: Response is empty");
    ESP_LOGE(TAG, "=== GET_SIGNED_URL FAILED ===");
    return false;
  }
}

bool ElevenLabsClient::connect(const std::string &signed_url,
                               std::function<void(const uint8_t *, size_t)> on_message,
                               std::function<void()> on_connected,
                               std::function<void()> on_disconnected,
                               std::function<void(const std::string &)> on_error) {
  if (!websocket_) return false;
  return websocket_->connect(signed_url, on_message, on_connected, on_disconnected, on_error);
}

void ElevenLabsClient::disconnect() {
  if (websocket_) websocket_->disconnect();
}

bool ElevenLabsClient::send_message(const std::string &message) {
  if (!websocket_) return false;
  return websocket_->send_message(message);
}

bool ElevenLabsClient::send_binary(const uint8_t *data, size_t length) {
  if (!websocket_) return false;
  return websocket_->send_binary(data, length);
}

bool ElevenLabsClient::is_connected() const {
  return websocket_ && websocket_->is_connected();
}

}  // namespace elevenlabs_stream
}  // namespace esphome

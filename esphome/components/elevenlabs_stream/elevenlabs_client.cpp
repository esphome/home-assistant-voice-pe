// elevenlabs_client.cpp
// Implements ElevenLabsClient: handles signed URL, HTTP, WebSocket connection, and protocol details.
#include "elevenlabs_client.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include "ws_big_reassembler.h"

namespace esphome {
namespace elevenlabs_stream {


static const char *TAG = "elevenlabs_client";
static const char *const ELEVENLABS_HOST = "api.elevenlabs.io";
static const int ELEVENLABS_PORT = 443;
static const char *const ELEVENLABS_SIGNED_URL_PATH = "/v1/convai/conversation/get_signed_url";

// Forward declaration for new WebsocketClient class
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

  // Configure HTTP client with ESP32 certificate bundle
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 3000;  // Reduced from 5000 to 3000ms
  config.method = HTTP_METHOD_GET;
  config.transport_type = HTTP_TRANSPORT_OVER_SSL;
  config.is_async = false;
  config.buffer_size = 1024;
  config.buffer_size_tx = 1024;

  // Use ESP32 built-in certificate bundle for SSL verification
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.use_global_ca_store = false;
  config.skip_cert_common_name_check = false;
  config.disable_auto_redirect = true;

  ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP client config prepared");
  ESP_LOGD(TAG, "GET_SIGNED_URL: Timeout=%dms, SSL=YES, Buffer=%d", config.timeout_ms, config.buffer_size);

  std::string response = "";

  // Static event handler for ESP-IDF
  auto http_event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
    std::string *response_ptr = static_cast<std::string *>(evt->user_data);
    switch (evt->event_id) {
      case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP_EVENT_ON_CONNECTED");
        esp_task_wdt_reset();
        break;
      case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (evt->data_len > 0 && response_ptr) {
          esp_task_wdt_reset();
          std::string response_chunk(static_cast<const char*>(evt->data), evt->data_len);
          (*response_ptr) += response_chunk;
          ESP_LOGD(TAG, "GET_SIGNED_URL: Received chunk: '%s'", response_chunk.c_str());
        }
        break;
      case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP_EVENT_ON_FINISH");
        esp_task_wdt_reset();
        break;
      case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "GET_SIGNED_URL: HTTP_EVENT_ERROR");
        esp_task_wdt_reset();
        break;
      case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP_EVENT_DISCONNECTED");
        esp_task_wdt_reset();
        break;
      default:
        ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP event %d", evt->event_id);
        break;
    }
    return ESP_OK;
  };
  config.event_handler = http_event_handler;
  config.user_data = &response;

  ESP_LOGD(TAG, "GET_SIGNED_URL: Initializing HTTP client with ESP32 certificate bundle");

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "GET_SIGNED_URL: Failed to initialize HTTP client");
    return false;
  }

  ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP client initialized successfully at %p", client);

  // Set headers only if API key is provided
  if (!this->api_key_.empty()) {
    ESP_LOGD(TAG, "GET_SIGNED_URL: Setting xi-api-key header");
    esp_err_t header_err = esp_http_client_set_header(client, "xi-api-key", this->api_key_.c_str());
    if (header_err != ESP_OK) {
      ESP_LOGE(TAG, "GET_SIGNED_URL: Failed to set API key header: %s", esp_err_to_name(header_err));
    } else {
      ESP_LOGD(TAG, "GET_SIGNED_URL: API key header set successfully");
    }
  } else {
    ESP_LOGD(TAG, "GET_SIGNED_URL: No API key provided, using public agent");
  }

  ESP_LOGD(TAG, "GET_SIGNED_URL: Sending GET request to signed URL endpoint");

  // Feed watchdog before and during HTTP request
  esp_task_wdt_reset();

  // Perform the request with timeout handling
  esp_err_t err = esp_http_client_perform(client);

  // Feed watchdog after request completion
  esp_task_wdt_reset();

  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP response code: %d", status_code);
  } else {
    ESP_LOGE(TAG, "GET_SIGNED_URL: HTTP request failed: %s", esp_err_to_name(err));
  }

  ESP_LOGD(TAG, "GET_SIGNED_URL: Cleaning up HTTP client");
  esp_http_client_cleanup(client);

  if (!response.empty()) {
    ESP_LOGD(TAG, "GET_SIGNED_URL: Parsing JSON response...");
    ESP_LOGD(TAG, "GET_SIGNED_URL: Full response: %s", response.c_str());

    // Parse JSON response using ESPHome's JSON utility
    bool parse_success = json::parse_json(response, [this, &signed_url_out](JsonObject root) -> bool {
      const char *signed_url = root["signed_url"];
      if (signed_url) {
        signed_url_out = std::string(signed_url);
        ESP_LOGI(TAG, "GET_SIGNED_URL: Extracted signed URL: %s", signed_url_out.c_str());
        return true;
      } else {
        ESP_LOGE(TAG, "GET_SIGNED_URL: signed_url field not found in response");
        ESP_LOGD(TAG, "GET_SIGNED_URL: Available fields in response:");
        for (JsonPair kv : root) {
          ESP_LOGD(TAG, "GET_SIGNED_URL:   - %s", kv.key().c_str());
        }
        return false;
      }
    });

    if (parse_success && !signed_url_out.empty()) {
      ESP_LOGI(TAG, "GET_SIGNED_URL: Got signed URL successfully");
      ESP_LOGD(TAG, "=== GET_SIGNED_URL SUCCESS ===");
      return true;
    } else {
      ESP_LOGE(TAG, "GET_SIGNED_URL: Failed to parse JSON response or extract signed_url");
    }
  } else {
    ESP_LOGE(TAG, "GET_SIGNED_URL: Response is empty");
  }

  ESP_LOGE(TAG, "=== GET_SIGNED_URL FAILED ===");
  return false;
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

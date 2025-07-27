// elevenlabs_client.cpp
// Implements ElevenLabsClient: handles signed URL, HTTP, WebSocket connection, and protocol details.
#include "elevenlabs_client.h"
#include "esphome/core/log.h"
#include "esphome/components/json/json_util.h"
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

namespace esphome {
namespace elevenlabs_stream {

static const char *TAG = "elevenlabs_client";
static const char *const ELEVENLABS_HOST = "api.elevenlabs.io";
static const int ELEVENLABS_PORT = 443;
static const char *const ELEVENLABS_SIGNED_URL_PATH = "/v1/convai/conversation/get_signed_url";

ElevenLabsClient::ElevenLabsClient(const std::string &agent_id, const std::string &api_key)
    : agent_id_(agent_id), api_key_(api_key) {}

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

  // Set event handler to capture response data
  config.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
    ElevenLabsStream *stream = static_cast<ElevenLabsStream *>(evt->user_data);

    switch (evt->event_id) {
      case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP_EVENT_ON_CONNECTED");
        esp_task_wdt_reset();
        break;
      case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (evt->data_len > 0) {
          // Feed watchdog during data reception
          esp_task_wdt_reset();
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
  config.user_data = this;

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

  std::string response = "";

  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "GET_SIGNED_URL: HTTP response code: %d", status_code);

    if (status_code == 200) {
      // Response data should be in signed_url_ from the event handler
      ESP_LOGI(TAG, "GET_SIGNED_URL: Response: %s", signed_url_out.c_str());
    } else {
      ESP_LOGE(TAG, "GET_SIGNED_URL: HTTP request failed with status code: %d", status_code);
    }
  } else {
    ESP_LOGE(TAG, "GET_SIGNED_URL: HTTP request failed: %s", esp_err_to_name(err));
  }

  ESP_LOGD(TAG, "GET_SIGNED_URL: Cleaning up HTTP client");
  esp_http_client_cleanup(client);

  if (!response.empty()) {
    ESP_LOGD(TAG, "GET_SIGNED_URL: Parsing JSON response...");
    ESP_LOGD(TAG, "GET_SIGNED_URL: Full response: %s", response.c_str());

    // Parse JSON response using ESPHome's JSON utility
    bool parse_success = json::parse_json(response, [this, signed_url_out](JsonObject root) -> bool {
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

    if (parse_success && !this->signed_url_.empty()) {
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

bool ElevenLabsClient::connect(const std::string &signed_url, std::function<void(const uint8_t *, size_t)> on_message,
                               std::function<void()> on_connected, std::function<void()> on_disconnected,
                               std::function<void(const std::string &)> on_error) {
  if (signed_url.empty()) {
    ESP_LOGE(TAG, "No signed URL provided");
    return false;
  }
  if (websocket_client_) {
    disconnect();
  }
  on_message_ = on_message;
  on_connected_ = on_connected;
  on_disconnected_ = on_disconnected;
  on_error_ = on_error;

  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = signed_url.c_str();
  ws_cfg.buffer_size = 4096;
  ws_cfg.task_stack = 8192;
  ws_cfg.task_prio = 1;
  ws_cfg.disable_auto_reconnect = true;
  ws_cfg.user_context = this;
  ws_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
  ws_cfg.network_timeout_ms = 10000;
  ws_cfg.reconnect_timeout_ms = 5000;
  ws_cfg.cert_pem = nullptr;
  ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
  ws_cfg.use_global_ca_store = false;
  ws_cfg.skip_cert_common_name_check = false;

  websocket_client_ = esp_websocket_client_init(&ws_cfg);
  if (!websocket_client_) {
    ESP_LOGE(TAG, "Failed to initialize WebSocket client");
    return false;
  }
  esp_err_t reg_err = esp_websocket_register_events(websocket_client_, WEBSOCKET_EVENT_ANY,
                                                    &ElevenLabsClient::websocket_event_handler, this);
  if (reg_err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to register WebSocket events");
    esp_websocket_client_destroy(websocket_client_);
    websocket_client_ = nullptr;
    return false;
  }
  esp_err_t err = esp_websocket_client_start(websocket_client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start WebSocket client");
    esp_websocket_client_destroy(websocket_client_);
    websocket_client_ = nullptr;
    return false;
  }
  return true;
}

void ElevenLabsClient::disconnect() {
  if (websocket_client_) {
    esp_websocket_client_stop(websocket_client_);
    esp_websocket_client_destroy(websocket_client_);
    websocket_client_ = nullptr;
    websocket_connected_ = false;
  }
}

bool ElevenLabsClient::send_message(const std::string &message) {
  if (!websocket_connected_ || !websocket_client_ || message.empty()) {
    return false;
  }
  int sent = esp_websocket_client_send_text(websocket_client_, message.c_str(), message.length(), portMAX_DELAY);
  return sent >= 0;
}

bool ElevenLabsClient::send_binary(const uint8_t *data, size_t length) {
  if (!websocket_connected_ || !websocket_client_ || !data || length == 0) {
    return false;
  }
  int sent = esp_websocket_client_send_bin(websocket_client_, (const char *) data, length, portMAX_DELAY);
  return sent >= 0;
}

bool ElevenLabsClient::is_connected() const { return websocket_connected_; }

void ElevenLabsClient::websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id,
                                               void *event_data) {
  ElevenLabsClient *client = static_cast<ElevenLabsClient *>(handler_args);
  if (event_id == WEBSOCKET_EVENT_DATA) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *) event_data;
    ElevenLabsClient *client = (ElevenLabsClient *) handler_args;
    if (client && client->on_message_) {
      client->on_message_(reinterpret_cast<const uint8_t *>(data->data_ptr), data->data_len);
    }
  }
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      client->websocket_connected_ = true;
      if (client->on_connected_)
        client->on_connected_();
      break;
    case WEBSOCKET_EVENT_DISCONNECTED:
      client->websocket_connected_ = false;
      if (client->on_disconnected_)
        client->on_disconnected_();
      break;
    case WEBSOCKET_EVENT_ERROR:
      if (client->on_error_)
        client->on_error_("WebSocket connection error");
      break;
    default:
      break;
  }
}

}  // namespace elevenlabs_stream
}  // namespace esphome

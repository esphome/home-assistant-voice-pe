#include "elevenlabs_stream.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/json/json_util.h"

#ifdef USE_ESP32
#include <esp_websocket_client.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <mbedtls/base64.h>

namespace esphome {
namespace elevenlabs_stream {

using namespace esphome::json;

static const char *const TAG = "elevenlabs_stream";

// ElevenLabs API endpoints
static const char *const ELEVENLABS_HOST = "api.elevenlabs.io";
static const int ELEVENLABS_PORT = 443;
static const char *const ELEVENLABS_SIGNED_URL_PATH = "/v1/convai/conversation/get_signed_url";

// ElevenLabs API SSL Certificate (ISRG Root X1 - Let's Encrypt root CA)
const char* elevenlabs_root_ca = \
  "-----BEGIN CERTIFICATE-----\n" \
  "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\n" \
  "-----END CERTIFICATE-----\n";

// Forward declaration for WebSocket event handler
void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

void ElevenLabsStream::setup() {
  ESP_LOGCONFIG(TAG, "Setting up ElevenLabs Stream...");
  
  // Microphone and speaker are optional for testing
  if (!this->microphone_) {
    ESP_LOGW(TAG, "Microphone not configured - audio capture disabled");
  }
  
  if (!this->speaker_) {
    ESP_LOGW(TAG, "Speaker not configured - audio playback disabled");
  }
  
  if (this->agent_id_.empty()) {
    ESP_LOGE(TAG, "Agent ID not configured");
    this->mark_failed();
    return;
  }
}

void ElevenLabsStream::dump_config() {
  ESP_LOGCONFIG(TAG, "ElevenLabs Stream:");
  ESP_LOGCONFIG(TAG, "  Agent ID: %s", this->agent_id_.c_str());
}

void ElevenLabsStream::loop() {
  // Handle connection state
  if (this->state_ == StreamState::CONNECTING) {
    // Check for connection timeout
    if (millis() - this->connection_start_time_ > this->connection_timeout_) {
      this->handle_error("Connection timeout");
      return;
    }
  }
  
  // Handle microphone audio when listening
  if (this->state_ == StreamState::LISTENING && this->microphone_) {
    if (millis() - this->last_audio_time_ > 100) { // Send audio every 100ms
      this->capture_and_send_audio();
      this->last_audio_time_ = millis();
    }
  }
  
  // Send periodic heartbeat
  if (this->websocket_connected_ && millis() - this->last_heartbeat_ > 30000) {
    this->send_ping();
    this->last_heartbeat_ = millis();
  }
}

bool ElevenLabsStream::start_stream() {
  if (this->state_ != StreamState::IDLE) {
    ESP_LOGW(TAG, "Cannot start stream - already running");
    return false;
  }
  
  ESP_LOGI(TAG, "Starting ElevenLabs stream...");
  this->set_state(StreamState::CONNECTING);
  this->connection_start_time_ = millis();
  
  // First get the signed URL, then connect
  if (!this->get_signed_url()) {
    this->handle_error("Failed to get signed URL");
    return false;
  }
  
  this->connect_to_elevenlabs();
  return true;
}

void ElevenLabsStream::stop_stream() {
  ESP_LOGI(TAG, "Stopping ElevenLabs stream...");
  this->disconnect_from_elevenlabs();
  this->set_state(StreamState::IDLE);
}

bool ElevenLabsStream::get_signed_url() {
  ESP_LOGI(TAG, "Getting signed URL from ElevenLabs...");
  
  if (this->api_key_.empty() || this->agent_id_.empty()) {
    ESP_LOGE(TAG, "API key or Agent ID not configured");
    return false;
  }
  
  // Create the URL
  std::string url = "https://";
  url += ELEVENLABS_HOST;
  url += ELEVENLABS_SIGNED_URL_PATH;
  
  ESP_LOGD(TAG, "Getting signed URL from: %s", url.c_str());
  
  // Configure HTTP client with ESP32 certificate bundle
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 10000;
  config.method = HTTP_METHOD_POST;
  config.transport_type = HTTP_TRANSPORT_OVER_SSL;
  config.is_async = false;
  
  // Use ESP32 built-in certificate bundle for SSL verification
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.use_global_ca_store = false;
  config.skip_cert_common_name_check = false;
  config.disable_auto_redirect = true;
  
  ESP_LOGD(TAG, "Initializing HTTP client with ESP32 certificate bundle");
  
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    return false;
  }
  
  // Set headers
  esp_http_client_set_header(client, "Content-Type", "application/json");
  std::string auth_header = "Bearer " + this->api_key_;
  esp_http_client_set_header(client, "Authorization", auth_header.c_str());
  
  // Create the JSON payload
  std::string payload = "{\"agent_id\":\"" + this->agent_id_ + "\"}";
  
  ESP_LOGD(TAG, "Sending request with payload: %s", payload.c_str());
  
  // Set POST data
  esp_http_client_set_post_field(client, payload.c_str(), payload.length());
  
  // Perform the request
  esp_err_t err = esp_http_client_perform(client);
  
  std::string response = "";
  
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    int content_length = esp_http_client_get_content_length(client);
    
    ESP_LOGD(TAG, "HTTP response code: %d, content length: %d", status_code, content_length);
    
    if (status_code == 200 && content_length > 0) {
      // Read response data
      char *buffer = (char*)malloc(content_length + 1);
      if (buffer) {
        int read_len = esp_http_client_read_response(client, buffer, content_length);
        if (read_len > 0) {
          buffer[read_len] = '\0';
          response = std::string(buffer);
          ESP_LOGD(TAG, "Response: %s", response.c_str());
        }
        free(buffer);
      }
    } else {
      ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
    }
  } else {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
  }
  
  esp_http_client_cleanup(client);
  
  if (!response.empty()) {
    // Parse JSON response to get signed_url
    // Simple parsing - look for "signed_url":"..." pattern
    size_t start = response.find("\"signed_url\":\"");
    if (start != std::string::npos) {
      start += 14; // Length of "signed_url":""
      size_t end = response.find("\"", start);
      if (end != std::string::npos) {
        std::string signed_url = response.substr(start, end - start);
        ESP_LOGD(TAG, "Extracted signed URL: %s", signed_url.c_str());
        this->signed_url_ = signed_url;
        ESP_LOGI(TAG, "Got signed URL successfully");
        return true;
      }
    }
    ESP_LOGE(TAG, "Failed to extract signed_url from response");
  }
  
  return false;
}

void ElevenLabsStream::connect_to_elevenlabs() {
  ESP_LOGI(TAG, "Connecting to ElevenLabs using signed URL...");
  
  if (this->signed_url_.empty()) {
    this->handle_error("No signed URL available");
    return;
  }
  
  ESP_LOGI(TAG, "Connecting to WebSocket URL: %s", this->signed_url_.c_str());
  
  // Configure WebSocket client with ESP32 certificate bundle
  esp_websocket_client_config_t ws_cfg = {};
  ws_cfg.uri = this->signed_url_.c_str();
  ws_cfg.buffer_size = 4096;
  ws_cfg.task_stack = 8192;
  ws_cfg.task_prio = 5;
  ws_cfg.disable_auto_reconnect = true;
  ws_cfg.user_context = this;  // Pass this instance as context
  ws_cfg.transport = WEBSOCKET_TRANSPORT_OVER_SSL;
  
  // Use ESP32 built-in certificate bundle for WebSocket SSL verification
  ws_cfg.cert_pem = nullptr;  // Use default certificate bundle
  ws_cfg.use_global_ca_store = false;
  ws_cfg.skip_cert_common_name_check = false;
  
  // Initialize WebSocket client
  this->websocket_client_ = esp_websocket_client_init(&ws_cfg);
  if (!this->websocket_client_) {
    this->handle_error("Failed to initialize WebSocket client");
    return;
  }
  
  // Register event handler
  esp_websocket_register_events(this->websocket_client_, WEBSOCKET_EVENT_ANY, &websocket_event_handler, this);
  
  // Start WebSocket connection
  esp_err_t err = esp_websocket_client_start(this->websocket_client_);
  if (err != ESP_OK) {
    this->handle_error("Failed to start WebSocket client");
    esp_websocket_client_destroy(this->websocket_client_);
    this->websocket_client_ = nullptr;
    return;
  }
  
  ESP_LOGI(TAG, "WebSocket client started, waiting for connection...");
  this->connection_start_time_ = millis();
}

void ElevenLabsStream::disconnect_from_elevenlabs() {
  ESP_LOGD(TAG, "Disconnecting from ElevenLabs...");
  this->websocket_connected_ = false;
  
  if (this->websocket_client_) {
    esp_websocket_client_stop(this->websocket_client_);
    esp_websocket_client_destroy(this->websocket_client_);
    this->websocket_client_ = nullptr;
  }
}

void ElevenLabsStream::set_state(StreamState new_state) {
  if (this->state_ == new_state) {
    return;
  }
  
  StreamState old_state = this->state_;
  this->state_ = new_state;
  
  ESP_LOGD(TAG, "State changed from %d to %d", static_cast<int>(old_state), static_cast<int>(new_state));
}

void ElevenLabsStream::send_websocket_message(const std::string &message) {
  if (!this->websocket_connected_ || !this->websocket_client_) {
    ESP_LOGW(TAG, "Cannot send message - WebSocket not connected");
    return;
  }
  
  int sent = esp_websocket_client_send_text(this->websocket_client_, message.c_str(), message.length(), portMAX_DELAY);
  if (sent < 0) {
    ESP_LOGE(TAG, "Failed to send WebSocket message");
  } else {
    ESP_LOGD(TAG, "Sent WebSocket message: %s", message.c_str());
  }
}

void ElevenLabsStream::handle_websocket_message(const char *message) {
  ESP_LOGD(TAG, "Received WebSocket message: %s", message);
  
  // Parse JSON message using ESPHome's JSON utility
  bool parse_success = json::parse_json(message, [this](JsonObject root) -> bool {
    const char* type = root["type"];
    if (!type) {
      ESP_LOGW(TAG, "Message missing type field");
      return true;
    }
    
    ESP_LOGD(TAG, "Message type: %s", type);
    
    if (strcmp(type, "conversation_initiation_metadata") == 0) {
      const char* conversation_id = root["conversation_id"];
      if (conversation_id) {
        this->conversation_id_ = conversation_id;
        ESP_LOGI(TAG, "Conversation initiated: %s", conversation_id);
        
        // Start listening
        this->set_state(StreamState::LISTENING);
        
        for (auto *trigger : this->on_start_triggers_) {
          trigger->trigger();
        }
        for (auto *trigger : this->on_listening_triggers_) {
          trigger->trigger();
        }
      }
    } else if (strcmp(type, "user_turn_started") == 0) {
      this->set_state(StreamState::SPEAKING);
      for (auto *trigger : this->on_speaking_triggers_) {
        trigger->trigger();
      }
    } else if (strcmp(type, "user_turn_ended") == 0) {
      this->set_state(StreamState::LISTENING);
      for (auto *trigger : this->on_listening_triggers_) {
        trigger->trigger();
      }
    } else if (strcmp(type, "error") == 0) {
      const char* error_msg = root["message"];
      this->handle_error(error_msg ? error_msg : "Unknown ElevenLabs error");
    } else if (strcmp(type, "ping") == 0) {
      // Send pong response
      std::string pong_message = json::build_json([](JsonObject root) {
        root["type"] = "pong";
      });
      this->send_websocket_message(pong_message);
    }
    
    return true;
  });
  
  if (!parse_success) {
    ESP_LOGE(TAG, "Failed to parse JSON message");
  }
}

void ElevenLabsStream::handle_websocket_binary(const uint8_t *data, size_t length) {
  ESP_LOGD(TAG, "Received binary audio data: %d bytes", length);
  // Placeholder - would handle audio data
}

void ElevenLabsStream::handle_audio_response(const uint8_t *data, size_t length) {
  ESP_LOGD(TAG, "Playing audio response: %d bytes", length);
  
  if (this->speaker_ && length > 0) {
    // Play audio through speaker - commented out for testing without speaker component
    // this->speaker_->play(data, length);
    ESP_LOGD(TAG, "Would play audio through speaker (disabled for testing)");
  }
}

void ElevenLabsStream::handle_error(const std::string &error_message) {
  ESP_LOGE(TAG, "Error: %s", error_message.c_str());
  this->set_state(StreamState::ERROR);
  
  for (auto *trigger : this->on_error_triggers_) {
    trigger->trigger(error_message);
  }
}

void ElevenLabsStream::websocket_task() {
  // This method is called from the loop to handle WebSocket communication
}

void ElevenLabsStream::send_conversation_init() {
  // Send initial conversation setup message using ESPHome's JSON builder
  std::string message = json::build_json([this](JsonObject root) {
    root["type"] = "conversation_initiation_client_data";
    
    JsonObject agent_config = root["agent_config"].to<JsonObject>();
    agent_config["agent_id"] = this->agent_id_;
    
    JsonObject conversation_config = root["conversation_config"].to<JsonObject>();
    conversation_config["audio_interface"] = "pcm_16000";
  });
  
  this->send_websocket_message(message);
}

void ElevenLabsStream::capture_and_send_audio() {
  if (!this->microphone_) {
    return;
  }
  
  // For now, just send empty audio data as a placeholder
  // This would need to be implemented based on the specific microphone interface
  ESP_LOGD(TAG, "Audio capture not implemented yet");
}

void ElevenLabsStream::send_ping() {
  // Send WebSocket ping frame using ESPHome's JSON builder
  std::string message = json::build_json([](JsonObject root) {
    root["type"] = "ping";
  });
  
  this->send_websocket_message(message);
}

void ElevenLabsStream::send_audio_chunk(const std::vector<int16_t> &audio_data) {
  if (!this->websocket_connected_ || !this->websocket_client_ || audio_data.empty()) {
    return;
  }
  
  // Send binary audio data
  const char* audio_bytes = reinterpret_cast<const char*>(audio_data.data());
  size_t audio_size = audio_data.size() * sizeof(int16_t);
  
  int sent = esp_websocket_client_send_bin(this->websocket_client_, audio_bytes, audio_size, portMAX_DELAY);
  if (sent < 0) {
    ESP_LOGE(TAG, "Failed to send audio chunk");
  } else {
    ESP_LOGV(TAG, "Sent audio chunk: %d samples", audio_data.size());
  }
}

// WebSocket event handler
void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  ElevenLabsStream *stream = static_cast<ElevenLabsStream*>(handler_args);
  esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t*>(event_data);
  
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WebSocket connected");
      stream->websocket_connected_ = true;
      stream->set_state(StreamState::CONNECTED);
      
      // Send initial conversation setup
      stream->send_conversation_init();
      
      // Trigger connected events
      for (auto *trigger : stream->on_connected_triggers_) {
        trigger->trigger();
      }
      break;
      
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "WebSocket disconnected");
      stream->websocket_connected_ = false;
      stream->set_state(StreamState::IDLE);
      
      for (auto *trigger : stream->on_disconnected_triggers_) {
        trigger->trigger();
      }
      break;
      
    case WEBSOCKET_EVENT_DATA:
      if (data->op_code == 0x01) { // Text frame
        std::string message(data->data_ptr, data->data_len);
        ESP_LOGD(TAG, "WebSocket text message: %s", message.c_str());
        stream->handle_websocket_message(message.c_str());
      } else if (data->op_code == 0x02) { // Binary frame
        ESP_LOGD(TAG, "WebSocket binary data: %d bytes", data->data_len);
        stream->handle_websocket_binary((const uint8_t*)data->data_ptr, data->data_len);
      }
      break;
      
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "WebSocket error");
      stream->handle_error("WebSocket connection error");
      break;
      
    default:
      break;
  }
}

}  // namespace elevenlabs_stream
}  // namespace esphome

#endif  // USE_ESP32

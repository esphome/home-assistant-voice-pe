#include "elevenlabs_stream.h"
#include "ws_big_reassembler.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/microphone/microphone.h"

#ifdef USE_ESP32
#include <esp_websocket_client.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <esp_task_wdt.h>
#include <mbedtls/base64.h>

namespace esphome {
namespace elevenlabs_stream {

using namespace esphome::json;

static const char* TAG = "elevenlabs_stream";

// Base64 encoding function with better error handling
std::string base64_encode(const uint8_t* data, size_t len) {
  if (!data || len == 0) {
    return "";
  }
  
  size_t output_len = 0;
  
  // Calculate required buffer size
  int ret = mbedtls_base64_encode(nullptr, 0, &output_len, data, len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    ESP_LOGE(TAG, "Failed to calculate base64 encode buffer size: %d", ret);
    return "";
  }
  
  // Allocate buffer and encode
  std::string result(output_len, '\0');
  ret = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&result[0]), output_len, &output_len, data, len);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to encode base64: %d", ret);
    return "";
  }
  
  result.resize(output_len);
  return result;
}

// ElevenLabs API endpoints
static const char *const ELEVENLABS_HOST = "api.elevenlabs.io";
static const int ELEVENLABS_PORT = 443;
static const char *const ELEVENLABS_SIGNED_URL_PATH = "/v1/convai/conversation/get_signed_url";

// Forward declaration for WebSocket event handler
void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

// Helper function to decode base64 audio data
std::vector<uint8_t> ElevenLabsStream::decode_base64_audio(const char* base64_data) {
  std::vector<uint8_t> decoded_data;
  
  if (!base64_data || strlen(base64_data) == 0) {
    return decoded_data;
  }
  
  size_t input_len = strlen(base64_data);
  size_t output_len = 0;
  
  // Calculate output length
  int ret = mbedtls_base64_decode(nullptr, 0, &output_len, (const unsigned char*)base64_data, input_len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    ESP_LOGE(TAG, "Failed to calculate base64 decode length");
    return decoded_data;
  }
  
  decoded_data.resize(output_len);
  
  // Actually decode
  ret = mbedtls_base64_decode(decoded_data.data(), output_len, &output_len, (const unsigned char*)base64_data, input_len);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to decode base64 audio data: %d", ret);
    decoded_data.clear();
    return decoded_data;
  }
  
  decoded_data.resize(output_len);
  ESP_LOGD(TAG, "Decoded %d bytes of audio data", output_len);
  
  return decoded_data;
}

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
  // Feed watchdog regularly during operation
  static uint32_t last_watchdog_feed = 0;
  if (millis() - last_watchdog_feed > 1000) { // Feed every second
    esp_task_wdt_reset();
    last_watchdog_feed = millis();
  }
  
  // Handle connection state
  if (this->state_ == StreamState::CONNECTING) {
    // Check for connection timeout
    if (millis() - this->connection_start_time_ > this->connection_timeout_) {
      this->handle_error("Connection timeout");
      return;
    }
  }
  
  // Handle state transition from SPEAKING back to LISTENING
  // If we're in SPEAKING state and haven't received audio for a while, go back to LISTENING
  if (this->state_ == StreamState::SPEAKING && millis() - this->last_audio_response_time_ > 2000) {
    ESP_LOGD(TAG, "No audio received for 2 seconds, returning to LISTENING state");
    this->set_state(StreamState::LISTENING);
    for (auto *trigger : this->on_listening_triggers_) {
      trigger->trigger();
    }
  }
  
  // Send periodic heartbeat (increase frequency for better connection reliability)
  if (this->websocket_connected_ && millis() - this->last_heartbeat_ > 20000) {  // 20 seconds instead of 30
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
  
  // Stop microphone if capturing
  if (this->microphone_ && this->microphone_->is_running()) {
    ESP_LOGD(TAG, "Stopping microphone capture");
    this->microphone_->stop();
  }
  
  // Stop speaker if running  
  if (this->speaker_ && this->speaker_->is_running()) {
    ESP_LOGD(TAG, "Stopping speaker");
    this->speaker_->stop();
  }
  
  this->disconnect_from_elevenlabs();
  this->set_state(StreamState::IDLE);
}

bool ElevenLabsStream::get_signed_url() {
  ESP_LOGI(TAG, "Getting signed URL from ElevenLabs...");
  
  if (this->agent_id_.empty()) {
    ESP_LOGE(TAG, "Agent ID not configured");
    return false;
  }
  
  // Create the URL with agent_id as query parameter
  std::string url = "https://";
  url += ELEVENLABS_HOST;
  url += ELEVENLABS_SIGNED_URL_PATH;
  url += "?agent_id=" + this->agent_id_;
  
  ESP_LOGD(TAG, "Getting signed URL from: %s", url.c_str());
  
  // Clear any previous signed URL
  this->signed_url_ = "";
  
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
  
  // Set event handler to capture response data
  config.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
    ElevenLabsStream *stream = static_cast<ElevenLabsStream*>(evt->user_data);
    
    switch (evt->event_id) {
      case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        esp_task_wdt_reset();
        break;
      case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (evt->data_len > 0) {
          // Feed watchdog during data reception
          esp_task_wdt_reset();
          // Append response data to signed_url_ temporarily
          std::string response_chunk(static_cast<const char*>(evt->data), evt->data_len);
          stream->signed_url_ += response_chunk;
        }
        break;
      case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        esp_task_wdt_reset();
        break;
      case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        esp_task_wdt_reset();
        break;
      case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        esp_task_wdt_reset();
        break;
      default:
        break;
    }
    return ESP_OK;
  };
  config.user_data = this;
  
  ESP_LOGD(TAG, "Initializing HTTP client with ESP32 certificate bundle");
  
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    return false;
  }
  
  // Set headers only if API key is provided
  if (!this->api_key_.empty()) {
    esp_http_client_set_header(client, "xi-api-key", this->api_key_.c_str());
  }
  
  ESP_LOGD(TAG, "Sending GET request to signed URL endpoint");
  
  // Feed watchdog before and during HTTP request
  esp_task_wdt_reset();
  
  // Perform the request with timeout handling
  esp_err_t err = esp_http_client_perform(client);
  
  // Feed watchdog after request completion
  esp_task_wdt_reset();
  
  std::string response = "";
  
  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGD(TAG, "HTTP response code: %d", status_code);
    
    if (status_code == 200) {
      // Response data should be in signed_url_ from the event handler
      response = this->signed_url_;
      this->signed_url_ = "";  // Clear it for proper parsing
      ESP_LOGI(TAG, "Response: %s", response.c_str());
    } else {
      ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
    }
  } else {
    ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
  }
  
  esp_http_client_cleanup(client);
  
  if (!response.empty()) {
    ESP_LOGD(TAG, "Parsing JSON response...");
    
    // Parse JSON response using ESPHome's JSON utility
    bool parse_success = json::parse_json(response, [this](JsonObject root) -> bool {
      const char* signed_url = root["signed_url"];
      if (signed_url) {
        this->signed_url_ = std::string(signed_url);
        ESP_LOGI(TAG, "Extracted signed URL: %s", this->signed_url_.c_str());
        return true;
      } else {
        ESP_LOGE(TAG, "signed_url field not found in response");
        return false;
      }
    });
    
    if (parse_success && !this->signed_url_.empty()) {
      ESP_LOGI(TAG, "Got signed URL successfully");
      return true;
    } else {
      ESP_LOGE(TAG, "Failed to parse JSON response or extract signed_url");
    }
  } else {
    ESP_LOGE(TAG, "Response is empty");
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
  ws_cfg.network_timeout_ms = 10000;  // 10 second timeout
  ws_cfg.reconnect_timeout_ms = 5000;  // 5 second reconnect timeout
  
  // Use ESP32 built-in certificate bundle for WebSocket SSL verification
  ws_cfg.cert_pem = nullptr;  // Use default certificate bundle
  ws_cfg.crt_bundle_attach = esp_crt_bundle_attach;
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
  
  // Stop microphone if capturing
  if (this->microphone_ && this->microphone_->is_running()) {
    this->microphone_->stop();
  }
  
  // Stop speaker if running
  if (this->speaker_ && this->speaker_->is_running()) {
    this->speaker_->stop();
  }
  
  // Clean up WebSocket client
  if (this->websocket_client_) {
    esp_websocket_client_stop(this->websocket_client_);
    esp_websocket_client_destroy(this->websocket_client_);
    this->websocket_client_ = nullptr;
  }
  
  // Clear buffers
  this->audio_buffer_.clear();
  this->response_audio_buffer_.clear();
  this->conversation_id_.clear();
  this->agent_output_audio_format_.clear();
  this->user_input_audio_format_.clear();
  
  // Trigger disconnected event
  for (auto *trigger : this->on_disconnected_triggers_) {
    trigger->trigger();
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
  if (!this->websocket_connected_ || !this->websocket_client_ || message.empty()) {
    ESP_LOGW(TAG, "Cannot send message - WebSocket not connected or message empty");
    return;
  }
  
  int sent = esp_websocket_client_send_text(this->websocket_client_, message.c_str(), message.length(), portMAX_DELAY);
  if (sent < 0) {
    ESP_LOGE(TAG, "Failed to send WebSocket message: %d", sent);
  } else {
    ESP_LOGV(TAG, "Sent WebSocket message (%d bytes): %s", sent, 
             message.length() > 200 ? (message.substr(0, 200) + "...").c_str() : message.c_str());
  }
}

void ElevenLabsStream::handle_websocket_message(const char *message) {
  if (!message || strlen(message) == 0) {
    ESP_LOGW(TAG, "Received empty WebSocket message");
    return;
  }
  
  size_t message_len = strlen(message);
  ESP_LOGD(TAG, "Received WebSocket message (%zu bytes): %s", message_len,
           message_len > 200 ? (std::string(message, 200) + "...").c_str() : message);
  
  // The ESP WebSocket client provides complete messages, so process directly
  this->parse_json_message(message);
}

void ElevenLabsStream::parse_json_message(const char *message) {
  bool parse_success = json::parse_json(message, [this](JsonObject root) -> bool {
        const char* type = root["type"];
        if (!type) {
          ESP_LOGW(TAG, "Message missing type field");
          return false;
        }
        
        ESP_LOGD(TAG, "Message type: %s", type);
        
        // Handle conversation_initiation_metadata
        if (strcmp(type, "conversation_initiation_metadata") == 0) {
          JsonObject metadata = root["conversation_initiation_metadata_event"];
          if (metadata) {
            const char* conversation_id = metadata["conversation_id"];
            const char* agent_output_format = metadata["agent_output_audio_format"];
            const char* user_input_format = metadata["user_input_audio_format"];
            
            if (conversation_id) {
              this->conversation_id_ = conversation_id;
              ESP_LOGI(TAG, "Conversation initiated: %s", conversation_id);
              
              // Store audio formats
              if (agent_output_format) {
                this->agent_output_audio_format_ = agent_output_format;
                ESP_LOGD(TAG, "Agent output format: %s", agent_output_format);
              }
              if (user_input_format) {
                this->user_input_audio_format_ = user_input_format;
                ESP_LOGD(TAG, "User input format: %s", user_input_format);
              }
              
              // Start listening
              this->set_state(StreamState::LISTENING);
              
              // Start microphone if available
              if (this->microphone_ && !this->microphone_->is_running()) {
                ESP_LOGD(TAG, "Starting microphone capture for listening");
                this->microphone_->start();
              }
              
              for (auto *trigger : this->on_start_triggers_) {
                trigger->trigger();
              }
              for (auto *trigger : this->on_listening_triggers_) {
                trigger->trigger();
              }
            }
          }
          return true;
        }
        
        // Handle audio events (corrected type name)
        if (strcmp(type, "audio") == 0) {
          JsonObject audio = root["audio_event"];
          if (audio) {
            const char* audio_base64 = audio["audio_base_64"];
            uint32_t event_id = audio["event_id"] | 0;
            
            if (audio_base64) {
              size_t base64_len = strlen(audio_base64);
              ESP_LOGD(TAG, "Received audio data (base64 length: %zu, event_id: %d)", base64_len, event_id);
              
              // Update timing for state management
              this->last_audio_response_time_ = millis();
              
              // Process audio chunks immediately for better real-time performance
              // Skip empty or very small chunks
              if (base64_len > 4) {
                // Decode base64 audio data and play it immediately
                std::vector<uint8_t> audio_data = this->decode_base64_audio(audio_base64);
                if (!audio_data.empty()) {
                  // Agent is speaking - change state
                  this->set_state(StreamState::SPEAKING);
                  for (auto *trigger : this->on_speaking_triggers_) {
                    trigger->trigger();
                  }
                  
                  // Play audio immediately to reduce latency
                  this->handle_audio_response(audio_data.data(), audio_data.size());
                }
              }
            }
          }
          return true;
        }
        
        // Handle user transcript
        if (strcmp(type, "user_transcript") == 0) {
          JsonObject transcript = root["user_transcription_event"];
          if (transcript) {
            const char* user_transcript = transcript["user_transcript"];
            if (user_transcript) {
              ESP_LOGI(TAG, "User transcript: %s", user_transcript);
              // Could trigger an event here for transcript handling
            }
          }
          return true;
        }
        
        // Handle agent response
        if (strcmp(type, "agent_response") == 0) {
          JsonObject response = root["agent_response_event"];
          if (response) {
            const char* agent_response = response["agent_response"];
            if (agent_response) {
              ESP_LOGI(TAG, "Agent response: %s", agent_response);
              // Could trigger an event here for response handling
            }
          }
          return true;
        }
        
        // Handle internal tentative agent response
        if (strcmp(type, "internal_tentative_agent_response") == 0) {
          JsonObject tentative = root["tentative_agent_response_internal_event"];
          if (tentative) {
            const char* tentative_response = tentative["tentative_agent_response"];
            if (tentative_response) {
              ESP_LOGD(TAG, "Tentative agent response: %s", tentative_response);
            }
          }
          return true;
        }
        
        // Handle VAD score
        if (strcmp(type, "vad_score") == 0) {
          JsonObject vad = root["vad_score_event"];
          if (vad) {
            float vad_score = vad["vad_score"] | 0.0f;
            ESP_LOGV(TAG, "VAD score: %.2f", vad_score);
            // Could use this for voice activity detection
          }
          return true;
        }
        
        // Handle interruption
        if (strcmp(type, "interruption") == 0) {
          JsonObject interruption = root["interruption_event"];
          if (interruption) {
            ESP_LOGD(TAG, "Interruption event received");
            // Handle interruption logic - could stop current audio playback
            if (this->speaker_) {
              this->speaker_->stop();
            }
          }
          return true;
        }
        
        // Handle agent_response_correction
        if (strcmp(type, "agent_response_correction") == 0) {
          JsonObject correction = root["agent_response_correction_event"];
          if (correction) {
            const char* corrected_text = correction["corrected_text"];
            if (corrected_text) {
              ESP_LOGD(TAG, "Agent response correction: %s", corrected_text);
            }
          }
          return true;
        }
        
        // Handle ping with proper response
        if (strcmp(type, "ping") == 0) {
          JsonObject ping = root["ping_event"];
          if (ping) {
            uint32_t event_id = ping["event_id"] | 0;
            uint32_t ping_ms = ping["ping_ms"] | 0;
            
            ESP_LOGD(TAG, "Ping received: event_id=%d, ping_ms=%d", event_id, ping_ms);
            
            // Send pong response with event_id
            std::string pong_message = json::build_json([event_id](JsonObject root) {
              root["type"] = "pong";
              root["event_id"] = event_id;
            });
            this->send_websocket_message(pong_message);
          }
          return true;
        }
        
        // Handle client tool call
        if (strcmp(type, "client_tool_call") == 0) {
          JsonObject tool_call = root["client_tool_call"];
          if (tool_call) {
            const char* tool_name = tool_call["tool_name"];
            const char* tool_call_id = tool_call["tool_call_id"];
            JsonObject parameters = tool_call["parameters"];
            
            if (tool_name && tool_call_id) {
              ESP_LOGI(TAG, "Client tool call: %s (id: %s)", tool_name, tool_call_id);
              // Handle tool calls - would need to implement tool handling
            }
          }
          return true;
        }
        
        // Handle contextual update
        if (strcmp(type, "contextual_update") == 0) {
          const char* text = root["text"];
          if (text) {
            ESP_LOGD(TAG, "Contextual update: %s", text);
          }
          return true;
        }
        
        // Log unknown message types for debugging
        ESP_LOGW(TAG, "Unknown message type: %s", type);
        return true;
      });
      
      if (!parse_success) {
        ESP_LOGE(TAG, "Failed to parse JSON message: %s", message);
      }
}

void ElevenLabsStream::handle_websocket_binary(const uint8_t *data, size_t length) {
  ESP_LOGD(TAG, "Received binary data: %d bytes", length);
  // ElevenLabs API uses JSON with base64 encoded audio, not binary frames
  // This method is kept for completeness but may not be used by ElevenLabs
  ESP_LOGW(TAG, "Binary WebSocket frames not expected in ElevenLabs protocol");
}

void ElevenLabsStream::handle_audio_response(const uint8_t *data, size_t length) {
  ESP_LOGD(TAG, "Playing audio response: %d bytes", length);
  
  if (!data || length == 0) {
    ESP_LOGD(TAG, "No audio data to play");
    return;
  }
  
  if (!this->speaker_) {
    ESP_LOGD(TAG, "No speaker configured");
    return;
  }
  
  // Start speaker if not running
  if (!this->speaker_->is_running()) {
    ESP_LOGD(TAG, "Starting speaker");
    this->speaker_->start();
  }
  
  // Play audio through speaker - make a copy to ensure data lifetime
  std::vector<uint8_t> audio_copy(data, data + length);
  this->speaker_->play(audio_copy.data(), audio_copy.size());
  ESP_LOGD(TAG, "Audio sent to speaker");
  
  // Note: We don't immediately return to LISTENING state here since
  // there might be more audio chunks coming. The state will be managed
  // by the timing of audio events and VAD scores.
}

void ElevenLabsStream::handle_error(const std::string &error_message) {
  ESP_LOGE(TAG, "Error: %s", error_message.c_str());
  this->set_state(StreamState::ERROR);
  
  for (auto *trigger : this->on_error_triggers_) {
    trigger->trigger(error_message);
  }
}

void ElevenLabsStream::send_conversation_init() {
  // Send initial conversation setup message using ESPHome's JSON builder
  std::string message = json::build_json([this](JsonObject root) {
    root["type"] = "conversation_initiation_client_data";
    
    // Add conversation_config_override as specified in the documentation
    JsonObject conversation_config = root["conversation_config_override"].to<JsonObject>();
    
    // Agent configuration
    JsonObject agent = conversation_config["agent"].to<JsonObject>();
    
    // TTS configuration
    JsonObject tts = conversation_config["tts"].to<JsonObject>();
    
    // Audio interface configuration
    conversation_config["audio_interface"] = "pcm_16000";
    
    // Turn detection configuration
    JsonObject turn_detection = conversation_config["turn_detection"].to<JsonObject>();
    turn_detection["type"] = "server_vad";
    
    // Language configuration
    conversation_config["language"] = "en";
  });
  
  ESP_LOGD(TAG, "Sending conversation init: %s", message.c_str());
  this->send_websocket_message(message);
}

void ElevenLabsStream::send_ping() {
  // Send WebSocket ping frame using ESPHome's JSON builder with proper event_id and timing
  static uint32_t ping_event_id = 1;
  uint32_t ping_ms = millis();
  
  uint32_t current_event_id = ping_event_id++;
  
  std::string message = json::build_json([current_event_id, ping_ms](JsonObject root) {
    root["type"] = "ping";
    JsonObject ping_event = root["ping_event"].to<JsonObject>();
    ping_event["event_id"] = current_event_id;
    ping_event["ping_ms"] = ping_ms;
  });
  
  ESP_LOGV(TAG, "Sending ping with event_id: %d", current_event_id);
  this->send_websocket_message(message);
}

void ElevenLabsStream::send_audio_chunk(const std::vector<int16_t> &audio_data) {
  if (!this->websocket_connected_ || !this->websocket_client_ || audio_data.empty()) {
    return;
  }
  
  // Convert audio data to bytes
  const uint8_t* audio_bytes = reinterpret_cast<const uint8_t*>(audio_data.data());
  size_t audio_size = audio_data.size() * sizeof(int16_t);
  
  // Encode audio as base64 for WebSocket transmission
  std::string audio_base64 = base64_encode(audio_bytes, audio_size);
  
  if (audio_base64.empty()) {
    ESP_LOGE(TAG, "Failed to encode audio data to base64");
    return;
  }
  
  // Send as user_audio_chunk according to protocol
  std::string message = json::build_json([&audio_base64](JsonObject root) {
    root["user_audio_chunk"] = audio_base64;
  });
  
  ESP_LOGV(TAG, "Sending audio chunk: %d samples, %d bytes, base64 length: %d", 
           audio_data.size(), audio_size, audio_base64.length());
  
  this->send_websocket_message(message);
}

void ElevenLabsStream::handle_microphone_data(const std::vector<uint8_t> &data) {
  if (this->state_ != StreamState::LISTENING || !this->websocket_connected_ || data.empty()) {
    return;
  }
  
  // Ensure data size is even (each sample is 2 bytes)
  if (data.size() % 2 != 0) {
    ESP_LOGW(TAG, "Received odd number of bytes for int16_t samples: %d", data.size());
    return;
  }
  
  // Convert uint8_t data to int16_t samples
  std::vector<int16_t> audio_samples;
  audio_samples.resize(data.size() / sizeof(int16_t));
  
  memcpy(audio_samples.data(), data.data(), data.size());
  
  ESP_LOGV(TAG, "Sending %d audio samples to ElevenLabs", audio_samples.size());
  
  // Send audio chunk to ElevenLabs
  this->send_audio_chunk(audio_samples);
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
      ESP_LOGD(TAG, "WebSocket data: opcode=%d, payload_len=%d, data_len=%d, payload_offset=%d", 
               data->op_code, data->payload_len, data->data_len, data->payload_offset);
      
      if (data->op_code == 0x08) { // Close frame
        if (data->data_len >= 2) {
          uint16_t close_code = (data->data_ptr[0] << 8) | data->data_ptr[1];
          ESP_LOGW(TAG, "WebSocket close frame received with code=%d", close_code);
        } else {
          ESP_LOGW(TAG, "WebSocket close frame received");
        }
        // Handle close frame - this will trigger WEBSOCKET_EVENT_DISCONNECTED
        break;
      }
      
      if (data->op_code == 0x01) { // Text frame
        // Handle text messages with robust fragmentation support using reassembler
        if (data->data_len > 0) {
          // Use the reassembler to handle fragmentation
          bool complete = stream->reassembler_.add(data);
          
          ESP_LOGD(TAG, "Message fragment: offset=%d, len=%d, total=%d, fin=%d, complete=%d", 
                   data->payload_offset, data->data_len, data->payload_len, data->fin, complete);
          
          if (complete) {
            // Get the complete message - handle large messages more efficiently
            std::vector<uint8_t> message_bytes = stream->reassembler_.take();
            
            if (!message_bytes.empty()) {
              // Add null terminator to make it a valid C-string
              message_bytes.push_back('\0');
              
              // Convert to string and process
              std::string message(reinterpret_cast<const char*>(message_bytes.data()));
              ESP_LOGD(TAG, "Complete message assembled (%zu bytes): %s", 
                       message.length(), 
                       message.length() > 200 ? (message.substr(0, 200) + "...").c_str() : message.c_str());
              
              // Process complete message
              stream->handle_websocket_message(message.c_str());
            } else {
              // Message was too large or failed to allocate - process directly from reassembler buffer
              if (stream->reassembler_.isReady()) {
                ESP_LOGW(TAG, "Processing large message directly from buffer");
                
                // Get pointer to complete message in reassembler buffer
                const uint8_t* buffer_ptr = stream->reassembler_.getBuffer();
                size_t buffer_size = stream->reassembler_.getSize();
                
                if (buffer_ptr && buffer_size > 0 && buffer_size < 200000) { // Safety limit
                  // Create a temporary null-terminated string for processing
                  char* temp_buffer = (char*)malloc(buffer_size + 1);
                  if (temp_buffer) {
                    memcpy(temp_buffer, buffer_ptr, buffer_size);
                    temp_buffer[buffer_size] = '\0';
                    
                    ESP_LOGD(TAG, "Large message processed (%zu bytes): %s", 
                             buffer_size, 
                             buffer_size > 200 ? (std::string(temp_buffer, 200) + "...").c_str() : temp_buffer);
                    
                    // Process the message
                    stream->handle_websocket_message(temp_buffer);
                    
                    // Free the temporary buffer
                    free(temp_buffer);
                  } else {
                    ESP_LOGE(TAG, "Failed to allocate temporary buffer for large message");
                  }
                } else {
                  ESP_LOGE(TAG, "Large message buffer invalid or too large: ptr=%p, size=%zu", buffer_ptr, buffer_size);
                }
                
                // Reset the reassembler after processing
                stream->reassembler_.reset();
              } else {
                ESP_LOGW(TAG, "Reassembler not ready despite complete flag");
              }
            }
          }
        }
      } else if (data->op_code == 0x02) { // Binary frame
        ESP_LOGD(TAG, "WebSocket binary data: %d bytes", data->data_len);
        if (data->data_len > 0) {
          stream->handle_websocket_binary((const uint8_t*)data->data_ptr, data->data_len);
        }
      } else {
        ESP_LOGW(TAG, "Unsupported WebSocket opcode: 0x%02x", data->op_code);
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

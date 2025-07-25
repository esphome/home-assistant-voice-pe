#include "elevenlabs_stream.h"
#include "ws_big_reassembler.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/application.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/audio/audio.h"

#include <esp_websocket_client.h>
#include <esp_http_client.h>
#include <esp_tls.h>
#include <esp_crt_bundle.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
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
bool ElevenLabsStream::decode_and_play_base64_audio(const char* base64_data) {
  ESP_LOGI(TAG, "DECODE_B64: Starting base64 audio decode and play");
  ESP_LOGI(TAG, "DECODE_B64: Input pointer=%p", base64_data);
  
  if (!base64_data || strlen(base64_data) == 0) {
    ESP_LOGW(TAG, "DECODE_B64: No base64 data provided");
    return false;
  }

  size_t input_len = strlen(base64_data);
  ESP_LOGD(TAG, "DECODE_B64: Input length=%zu", input_len);
  
  size_t output_len = 0;
  
  // Calculate output length
  ESP_LOGD(TAG, "DECODE_B64: Calculating output length...");
  int ret = mbedtls_base64_decode(nullptr, 0, &output_len, (const unsigned char*)base64_data, input_len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    ESP_LOGE(TAG, "DECODE_B64: Failed to calculate base64 decode length: %d", ret);
    return false;
  }
  
  ESP_LOGD(TAG, "DECODE_B64: Required output length=%zu", output_len);
  
  // Use PSRAM allocation for large audio data to avoid regular heap exhaustion
  uint8_t* psram_buffer = (uint8_t*)heap_caps_malloc(output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!psram_buffer) {
    ESP_LOGE(TAG, "DECODE_B64: Failed to allocate PSRAM buffer for decoded audio (%zu bytes)", output_len);
    return false;
  }
  
  ESP_LOGD(TAG, "DECODE_B64: Allocated PSRAM buffer at %p for %zu bytes", psram_buffer, output_len);
  
  // Actually decode directly to PSRAM buffer
  ESP_LOGD(TAG, "DECODE_B64: Performing decode...");
  ret = mbedtls_base64_decode(psram_buffer, output_len, &output_len, (const unsigned char*)base64_data, input_len);
  if (ret != 0) {
    ESP_LOGE(TAG, "DECODE_B64: Failed to decode base64 audio data: %d", ret);
    heap_caps_free(psram_buffer);
    return false;
  }
  
  ESP_LOGD(TAG, "DECODE_B64: Decoded %zu bytes of audio data successfully", output_len);
  
  // Play audio directly from PSRAM buffer - no std::vector needed!
  ESP_LOGD(TAG, "DECODE_B64: Playing audio response directly from PSRAM buffer...");
  this->handle_audio_response(psram_buffer, output_len);
  
  // CRITICAL: Don't free the buffer until audio is finished playing!
  // Calculate estimated duration and schedule buffer cleanup
  size_t samples = output_len / 2; // 16-bit samples = 2 bytes each
  uint32_t sample_rate = 44100; // ElevenLabs default
  float duration_ms = (float)samples / (sample_rate / 1000.0f);
  
  // Account for resampler/mixer pipeline delays - these components buffer and process data
  // The resampler converts 44.1kHz->48kHz and the mixer combines streams
  // Add significant buffer time to account for pipeline delays (3-5 seconds total)
  uint32_t pipeline_delay_ms = 4000; // 4 second buffer for resampler/mixer pipeline
  uint32_t wait_time_ms = (uint32_t)(duration_ms + pipeline_delay_ms);
  
  ESP_LOGI(TAG, "DECODE_B64: Scheduling buffer cleanup for audio duration: %.1f ms + %u ms pipeline buffer = %u ms", 
           duration_ms, pipeline_delay_ms, wait_time_ms);
  
  // Schedule buffer cleanup without blocking the WebSocket task
  this->set_timeout("cleanup_audio_buffer", wait_time_ms, [psram_buffer]() {
    ESP_LOGI("elevenlabs_stream", "CLEANUP: Audio playback completed after pipeline delay, freeing PSRAM buffer");
    heap_caps_free(psram_buffer);
    ESP_LOGD("elevenlabs_stream", "CLEANUP: PSRAM buffer freed after audio completion");
  });
  
  return true;
}

void ElevenLabsStream::setup() {
  ESP_LOGCONFIG(TAG, "=== SETUP START ===");
  ESP_LOGCONFIG(TAG, "Setting up ElevenLabs Stream...");
  ESP_LOGD(TAG, "SETUP: Component instance created at %p", this);
  ESP_LOGD(TAG, "SETUP: Agent ID: '%s'", this->agent_id_.c_str());
  ESP_LOGD(TAG, "SETUP: API Key configured: %s", this->api_key_.empty() ? "NO" : "YES");
  
  // Microphone and speaker are optional for testing
  if (!this->microphone_) {
    ESP_LOGW(TAG, "SETUP: Microphone not configured - audio capture disabled");
  } else {
    ESP_LOGI(TAG, "SETUP: Microphone configured successfully at %p", this->microphone_);
  }
  
  if (!this->speaker_) {
    ESP_LOGW(TAG, "SETUP: Speaker not configured - audio playback disabled");
  } else {
    ESP_LOGI(TAG, "SETUP: Speaker configured successfully at %p", this->speaker_);
  }
  
  if (this->agent_id_.empty()) {
    ESP_LOGE(TAG, "SETUP: Agent ID not configured - SETUP FAILED");
    this->mark_failed();
    return;
  }
  
  ESP_LOGD(TAG, "SETUP: Initial state set to %d (IDLE)", static_cast<int>(this->state_));
  
  // Initialize signed URL at startup for speedy connections
  ESP_LOGI(TAG, "SETUP: Initializing signed URL for faster connections...");
  if (this->get_signed_url()) {
    this->signed_url_valid_ = true;
    this->last_signed_url_renewal_ = millis();
    ESP_LOGI(TAG, "SETUP: Signed URL initialized successfully");
  } else {
    ESP_LOGW(TAG, "SETUP: Failed to get initial signed URL - will retry in loop");
    this->signed_url_valid_ = false;
  }
  
  ESP_LOGCONFIG(TAG, "=== SETUP COMPLETE ===");
}

void ElevenLabsStream::dump_config() {
  ESP_LOGCONFIG(TAG, "ElevenLabs Stream:");
  ESP_LOGCONFIG(TAG, "  Agent ID: %s", this->agent_id_.c_str());
}

void ElevenLabsStream::loop() {
  // Feed watchdog regularly during operation
  static uint32_t last_watchdog_feed = 0;
  static uint32_t loop_count = 0;
  loop_count++;
  
  if (millis() - last_watchdog_feed > 1000) { // Feed every second
    esp_task_wdt_reset();
    last_watchdog_feed = millis();
    ESP_LOGD(TAG, "LOOP: Watchdog fed at loop count %d, state=%d", loop_count, static_cast<int>(this->state_));
  }
  
  // Handle connection state
  if (this->state_ == StreamState::CONNECTING) {
    uint32_t elapsed = millis() - this->connection_start_time_;
    ESP_LOGD(TAG, "LOOP: CONNECTING state - elapsed=%dms, timeout=%dms", elapsed, this->connection_timeout_);
    
    // Check for connection timeout
    if (elapsed > this->connection_timeout_) {
      ESP_LOGE(TAG, "LOOP: Connection timeout after %dms", elapsed);
      this->handle_error("Connection timeout");
      return;
    }
  }
  
  // Handle state transition from SPEAKING back to LISTENING
  // If we're in SPEAKING state and haven't received audio for a while, go back to LISTENING
  if (this->state_ == StreamState::SPEAKING) {
    uint32_t silence_duration = millis() - this->last_audio_response_time_;
    ESP_LOGD(TAG, "LOOP: SPEAKING state - silence duration=%dms", silence_duration);
    
    if (silence_duration > 2000) {
      ESP_LOGD(TAG, "LOOP: No audio received for %dms, returning to LISTENING state", silence_duration);
      this->set_state(StreamState::LISTENING);
      for (auto *trigger : this->on_listening_triggers_) {
        ESP_LOGD(TAG, "LOOP: Triggering listening event at %p", trigger);
        trigger->trigger();
      }
    }
  }
  
  // Send periodic heartbeat (increase frequency for better connection reliability)
  if (this->websocket_connected_) {
    uint32_t heartbeat_elapsed = millis() - this->last_heartbeat_;
    if (heartbeat_elapsed > 20000) {  // 20 seconds instead of 30
      ESP_LOGD(TAG, "LOOP: Sending heartbeat ping after %dms", heartbeat_elapsed);
      this->send_ping();
      this->last_heartbeat_ = millis();
    }
  }
  
  // Renew signed URL periodically for fast connections
  this->renew_signed_url_if_needed();
}

bool ElevenLabsStream::start_stream() {
  ESP_LOGI(TAG, "=== START_STREAM CALLED ===");
  ESP_LOGD(TAG, "START_STREAM: Current state=%d", static_cast<int>(this->state_));
  ESP_LOGD(TAG, "START_STREAM: WebSocket connected=%s", this->websocket_connected_ ? "YES" : "NO");
  ESP_LOGD(TAG, "START_STREAM: Agent ID='%s'", this->agent_id_.c_str());
  ESP_LOGD(TAG, "START_STREAM: Microphone=%p, Speaker=%p", this->microphone_, this->speaker_);
  ESP_LOGD(TAG, "START_STREAM: Signed URL valid=%s", this->signed_url_valid_ ? "YES" : "NO");
  
  if (this->state_ != StreamState::IDLE) {
    ESP_LOGW(TAG, "START_STREAM: Cannot start stream - already running (state=%d)", static_cast<int>(this->state_));
    return false;
  }
  
  ESP_LOGI(TAG, "START_STREAM: Starting ElevenLabs stream...");
  this->set_state(StreamState::CONNECTING);
  this->connection_start_time_ = millis();
  ESP_LOGD(TAG, "START_STREAM: Connection start time set to %d", this->connection_start_time_);
  
  // Use cached signed URL if available and valid, otherwise get a new one
  if (!this->signed_url_valid_ || this->signed_url_.empty()) {
    ESP_LOGD(TAG, "START_STREAM: No valid cached signed URL, getting new one...");
    if (!this->get_signed_url()) {
      ESP_LOGE(TAG, "START_STREAM: Failed to get signed URL");
      this->handle_error("Failed to get signed URL");
      return false;
    }
    this->signed_url_valid_ = true;
    this->last_signed_url_renewal_ = millis();
    ESP_LOGD(TAG, "START_STREAM: New signed URL obtained successfully");
  } else {
    ESP_LOGI(TAG, "START_STREAM: Using cached signed URL for fast connection");
  }
  
  ESP_LOGD(TAG, "START_STREAM: Connecting to ElevenLabs...");
  this->connect_to_elevenlabs();
  ESP_LOGD(TAG, "=== START_STREAM COMPLETE ===");
  return true;
}

void ElevenLabsStream::stop_stream() {
  ESP_LOGI(TAG, "=== STOP_STREAM CALLED ===");
  ESP_LOGD(TAG, "STOP_STREAM: Current state=%d", static_cast<int>(this->state_));
  ESP_LOGD(TAG, "STOP_STREAM: WebSocket connected=%s", this->websocket_connected_ ? "YES" : "NO");
  ESP_LOGI(TAG, "STOP_STREAM: Stopping ElevenLabs stream...");
  
  // Stop microphone if capturing
  if (this->microphone_ && this->microphone_->is_running()) {
    ESP_LOGD(TAG, "STOP_STREAM: Stopping microphone capture");
    this->microphone_->stop();
    ESP_LOGD(TAG, "STOP_STREAM: Microphone stopped");
  } else {
    ESP_LOGD(TAG, "STOP_STREAM: Microphone not running or not configured");
  }
  
  // Stop speaker if running  
  if (this->speaker_ && this->speaker_->is_running()) {
    ESP_LOGD(TAG, "STOP_STREAM: Stopping speaker");
    this->speaker_->stop();
    ESP_LOGD(TAG, "STOP_STREAM: Speaker stopped");
  } else {
    ESP_LOGD(TAG, "STOP_STREAM: Speaker not running or not configured");
  }
  
  ESP_LOGD(TAG, "STOP_STREAM: Disconnecting from ElevenLabs...");
  this->disconnect_from_elevenlabs();
  ESP_LOGD(TAG, "STOP_STREAM: Setting state to IDLE...");
  this->set_state(StreamState::IDLE);
  ESP_LOGD(TAG, "=== STOP_STREAM COMPLETE ===");
}

bool ElevenLabsStream::get_signed_url() {
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
  this->signed_url_ = "";
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
    ElevenLabsStream *stream = static_cast<ElevenLabsStream*>(evt->user_data);
    
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
          // Append response data to signed_url_ temporarily
          std::string response_chunk(static_cast<const char*>(evt->data), evt->data_len);
          stream->signed_url_ += response_chunk;
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
      response = this->signed_url_;
      this->signed_url_ = "";  // Clear it for proper parsing
      ESP_LOGI(TAG, "GET_SIGNED_URL: Response: %s", response.c_str());
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
    bool parse_success = json::parse_json(response, [this](JsonObject root) -> bool {
      const char* signed_url = root["signed_url"];
      if (signed_url) {
        this->signed_url_ = std::string(signed_url);
        ESP_LOGI(TAG, "GET_SIGNED_URL: Extracted signed URL: %s", this->signed_url_.c_str());
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

void ElevenLabsStream::renew_signed_url_if_needed() {
  // Skip renewal if we don't have a valid agent ID configured
  if (this->agent_id_.empty()) {
    return;
  }
  
  uint32_t current_time = millis();
  
  // Check if we need to renew the signed URL
  bool should_renew = false;
  
  // Renew if we don't have a valid signed URL
  if (!this->signed_url_valid_ || this->signed_url_.empty()) {
    ESP_LOGD(TAG, "RENEW: No valid signed URL available, will renew");
    should_renew = true;
  }
  // Renew if the renewal interval has passed
  else if (current_time - this->last_signed_url_renewal_ >= this->signed_url_renewal_interval_) {
    uint32_t elapsed_minutes = (current_time - this->last_signed_url_renewal_) / 60000;
    ESP_LOGI(TAG, "RENEW: Signed URL renewal interval reached (%d minutes elapsed)", elapsed_minutes);
    should_renew = true;
  }
  
  if (should_renew) {
    ESP_LOGI(TAG, "RENEW: Renewing signed URL for fast connections...");
    
    // Don't interrupt active connections - only renew when idle
    if (this->state_ == StreamState::IDLE) {
      if (this->get_signed_url()) {
        this->signed_url_valid_ = true;
        this->last_signed_url_renewal_ = current_time;
        ESP_LOGI(TAG, "RENEW: Signed URL renewed successfully");
      } else {
        ESP_LOGW(TAG, "RENEW: Failed to renew signed URL");
        this->signed_url_valid_ = false;
      }
    } else {
      ESP_LOGD(TAG, "RENEW: Deferring renewal - stream is active (state=%d)", static_cast<int>(this->state_));
    }
  }
}

void ElevenLabsStream::connect_to_elevenlabs() {
  ESP_LOGI(TAG, "=== CONNECT_TO_ELEVENLABS START ===");
  ESP_LOGI(TAG, "CONNECT: Connecting to ElevenLabs using signed URL...");
  ESP_LOGD(TAG, "CONNECT: Signed URL length=%zu", this->signed_url_.length());
  
  if (this->signed_url_.empty()) {
    ESP_LOGE(TAG, "CONNECT: No signed URL available");
    this->handle_error("No signed URL available");
    return;
  }
  
  ESP_LOGI(TAG, "CONNECT: Connecting to WebSocket URL: %s", this->signed_url_.c_str());
  
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
  
  ESP_LOGD(TAG, "CONNECT: WebSocket config prepared:");
  ESP_LOGD(TAG, "CONNECT:   - Buffer size: %d", ws_cfg.buffer_size);
  ESP_LOGD(TAG, "CONNECT:   - Task stack: %d", ws_cfg.task_stack);
  ESP_LOGD(TAG, "CONNECT:   - Task priority: %d", ws_cfg.task_prio);
  ESP_LOGD(TAG, "CONNECT:   - Network timeout: %dms", ws_cfg.network_timeout_ms);
  ESP_LOGD(TAG, "CONNECT:   - Auto reconnect: %s", ws_cfg.disable_auto_reconnect ? "NO" : "YES");
  ESP_LOGD(TAG, "CONNECT:   - Transport: SSL");
  
  // Initialize WebSocket client
  ESP_LOGD(TAG, "CONNECT: Initializing WebSocket client...");
  this->websocket_client_ = esp_websocket_client_init(&ws_cfg);
  if (!this->websocket_client_) {
    ESP_LOGE(TAG, "CONNECT: Failed to initialize WebSocket client");
    this->handle_error("Failed to initialize WebSocket client");
    return;
  }
  
  ESP_LOGD(TAG, "CONNECT: WebSocket client initialized at %p", this->websocket_client_);
  
  // Register event handler
  ESP_LOGD(TAG, "CONNECT: Registering WebSocket event handler...");
  esp_err_t reg_err = esp_websocket_register_events(this->websocket_client_, WEBSOCKET_EVENT_ANY, &websocket_event_handler, this);
  if (reg_err != ESP_OK) {
    ESP_LOGE(TAG, "CONNECT: Failed to register WebSocket events: %s", esp_err_to_name(reg_err));
    esp_websocket_client_destroy(this->websocket_client_);
    this->websocket_client_ = nullptr;
    this->handle_error("Failed to register WebSocket events");
    return;
  }
  
  ESP_LOGD(TAG, "CONNECT: Event handler registered successfully");
  
  // Start WebSocket connection
  ESP_LOGD(TAG, "CONNECT: Starting WebSocket client...");
  esp_err_t err = esp_websocket_client_start(this->websocket_client_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "CONNECT: Failed to start WebSocket client: %s", esp_err_to_name(err));
    this->handle_error("Failed to start WebSocket client");
    esp_websocket_client_destroy(this->websocket_client_);
    this->websocket_client_ = nullptr;
    return;
  }
  
  ESP_LOGI(TAG, "CONNECT: WebSocket client started, waiting for connection...");
  this->connection_start_time_ = millis();
  ESP_LOGD(TAG, "CONNECT: Connection start time updated to %d", this->connection_start_time_);
  ESP_LOGD(TAG, "=== CONNECT_TO_ELEVENLABS INITIATED ===");
}

void ElevenLabsStream::disconnect_from_elevenlabs() {
  ESP_LOGD(TAG, "=== DISCONNECT_FROM_ELEVENLABS START ===");
  ESP_LOGD(TAG, "DISCONNECT: Disconnecting from ElevenLabs...");
  ESP_LOGD(TAG, "DISCONNECT: Current state=%d", static_cast<int>(this->state_));
  ESP_LOGD(TAG, "DISCONNECT: WebSocket connected=%s", this->websocket_connected_ ? "YES" : "NO");
  ESP_LOGD(TAG, "DISCONNECT: WebSocket client=%p", this->websocket_client_);
  
  this->websocket_connected_ = false;
  
  // Stop microphone if capturing
  if (this->microphone_ && this->microphone_->is_running()) {
    ESP_LOGD(TAG, "DISCONNECT: Stopping microphone capture");
    this->microphone_->stop();
    ESP_LOGD(TAG, "DISCONNECT: Microphone stopped");
  } else {
    ESP_LOGD(TAG, "DISCONNECT: Microphone not running or not configured");
  }
  
  // Stop speaker if running
  if (this->speaker_ && this->speaker_->is_running()) {
    ESP_LOGD(TAG, "DISCONNECT: Stopping speaker");
    this->speaker_->stop();
    ESP_LOGD(TAG, "DISCONNECT: Speaker stopped");
  } else {
    ESP_LOGD(TAG, "DISCONNECT: Speaker not running or not configured");
  }
  
  // Clean up WebSocket client
  if (this->websocket_client_) {
    ESP_LOGD(TAG, "DISCONNECT: Stopping WebSocket client...");
    esp_err_t stop_err = esp_websocket_client_stop(this->websocket_client_);
    if (stop_err != ESP_OK) {
      ESP_LOGW(TAG, "DISCONNECT: WebSocket stop failed: %s", esp_err_to_name(stop_err));
    } else {
      ESP_LOGD(TAG, "DISCONNECT: WebSocket client stopped");
    }
    
    ESP_LOGD(TAG, "DISCONNECT: Destroying WebSocket client...");
    esp_err_t destroy_err = esp_websocket_client_destroy(this->websocket_client_);
    if (destroy_err != ESP_OK) {
      ESP_LOGW(TAG, "DISCONNECT: WebSocket destroy failed: %s", esp_err_to_name(destroy_err));
    } else {
      ESP_LOGD(TAG, "DISCONNECT: WebSocket client destroyed");
    }
    
    this->websocket_client_ = nullptr;
    ESP_LOGD(TAG, "DISCONNECT: WebSocket client pointer cleared");
  } else {
    ESP_LOGD(TAG, "DISCONNECT: No WebSocket client to clean up");
  }
  
  // Clear buffers
  ESP_LOGD(TAG, "DISCONNECT: Clearing buffers...");
  size_t audio_buffer_size = this->audio_buffer_.size();
  size_t response_audio_buffer_size = this->response_audio_buffer_.size();
  
  this->audio_buffer_.clear();
  this->response_audio_buffer_.clear();
  this->conversation_id_.clear();
  this->agent_output_audio_format_.clear();
  this->user_input_audio_format_.clear();
  
  ESP_LOGD(TAG, "DISCONNECT: Cleared audio_buffer (%zu bytes), response_audio_buffer (%zu bytes)", 
           audio_buffer_size, response_audio_buffer_size);
  ESP_LOGD(TAG, "DISCONNECT: Cleared conversation_id, audio formats");
  
  // Trigger disconnected event
  ESP_LOGD(TAG, "DISCONNECT: Triggering disconnected events...");
  for (auto *trigger : this->on_disconnected_triggers_) {
    ESP_LOGD(TAG, "DISCONNECT: Triggering disconnected event at %p", trigger);
    trigger->trigger();
  }
  ESP_LOGD(TAG, "DISCONNECT: All disconnected events triggered");
  ESP_LOGD(TAG, "=== DISCONNECT_FROM_ELEVENLABS COMPLETE ===");
}

void ElevenLabsStream::set_state(StreamState new_state) {
  if (this->state_ == new_state) {
    ESP_LOGD(TAG, "SET_STATE: State unchanged, still %d", static_cast<int>(new_state));
    return;
  }
  
  StreamState old_state = this->state_;
  this->state_ = new_state;
  
  ESP_LOGI(TAG, "STATE_CHANGE: %d -> %d (IDLE=0, CONNECTING=1, CONNECTED=2, LISTENING=3, SPEAKING=4, ERROR=5)", 
           static_cast<int>(old_state), static_cast<int>(new_state));
  
  // Log human-readable state names
  const char* old_state_name = "UNKNOWN";
  const char* new_state_name = "UNKNOWN";
  
  switch (old_state) {
    case StreamState::IDLE: old_state_name = "IDLE"; break;
    case StreamState::CONNECTING: old_state_name = "CONNECTING"; break;
    case StreamState::CONNECTED: old_state_name = "CONNECTED"; break;
    case StreamState::LISTENING: old_state_name = "LISTENING"; break;
    case StreamState::SPEAKING: old_state_name = "SPEAKING"; break;
    case StreamState::ERROR: old_state_name = "ERROR"; break;
  }
  
  switch (new_state) {
    case StreamState::IDLE: new_state_name = "IDLE"; break;
    case StreamState::CONNECTING: new_state_name = "CONNECTING"; break;
    case StreamState::CONNECTED: new_state_name = "CONNECTED"; break;
    case StreamState::LISTENING: new_state_name = "LISTENING"; break;
    case StreamState::SPEAKING: new_state_name = "SPEAKING"; break;
    case StreamState::ERROR: new_state_name = "ERROR"; break;
  }
  
  ESP_LOGI(TAG, "STATE_CHANGE: %s -> %s", old_state_name, new_state_name);
}

void ElevenLabsStream::send_websocket_message(const std::string &message) {
  if (!this->websocket_connected_ || !this->websocket_client_ || message.empty()) {
    ESP_LOGW(TAG, "SEND_WS_MSG: Cannot send message - WebSocket not connected or message empty");
    ESP_LOGW(TAG, "SEND_WS_MSG:   connected=%s, client=%p, empty=%s", 
             this->websocket_connected_ ? "YES" : "NO",
             this->websocket_client_,
             message.empty() ? "YES" : "NO");
    return;
  }
  
  int sent = esp_websocket_client_send_text(this->websocket_client_, message.c_str(), message.length(), portMAX_DELAY);
  if (sent < 0) {
    ESP_LOGE(TAG, "SEND_WS_MSG: Failed to send WebSocket message: %d", sent);
  }
}

void ElevenLabsStream::handle_websocket_message(const uint8_t *buffer, size_t length) {
  if (!buffer || length == 0) {
    ESP_LOGW(TAG, "HANDLE_WS_MSG: Received empty WebSocket message");
    return;
  }
  
  this->parse_json_message_from_buffer(buffer, length);
  ESP_LOGD(TAG, "HANDLE_WS_MSG: Message processing complete");
}

void ElevenLabsStream::parse_json_message_from_buffer(const uint8_t *buffer, size_t length) {
  // Use ArduinoJson directly with PSRAM allocator
  // Create a PSRAM allocator for BasicJsonDocument
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
  
  BasicJsonDocument<PSRAMAllocator> json_document(length + 1024); // Extra space for parsing overhead
  if (json_document.overflowed()) {
    ESP_LOGE(TAG, "PARSE_JSON_BUF: Could not allocate memory for JSON document!");
    return;
  }
  
  // Parse JSON directly from buffer
  DeserializationError err = deserializeJson(json_document, (const char*)buffer, length);
  
  if (err != DeserializationError::Ok) {
    if (err == DeserializationError::NoMemory) {
      ESP_LOGE(TAG, "PARSE_JSON_BUF: Can not allocate more memory for deserialization. Consider making source string smaller");
    } else {
      ESP_LOGE(TAG, "PARSE_JSON_BUF: Parse error: %s", err.c_str());
    }
    return;
  }
  
  JsonObject root = json_document.as<JsonObject>();
  const char* type = root["type"];
  if (!type) {
    ESP_LOGW(TAG, "PARSE_JSON_BUF: Message missing type field");
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Available root fields:");
    for (JsonPair kv : root) {
      ESP_LOGD(TAG, "PARSE_JSON_BUF:   - %s", kv.key().c_str());
    }
    return;
  }
  
  ESP_LOGI(TAG, "PARSE_JSON_BUF: Message type: '%s'", type);
  
  // Handle conversation_initiation_metadata
  if (strcmp(type, "conversation_initiation_metadata") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing conversation_initiation_metadata");
    JsonObject metadata = root["conversation_initiation_metadata_event"];
    if (metadata) {
      ESP_LOGD(TAG, "PARSE_JSON_BUF: Found conversation_initiation_metadata_event");
      const char* conversation_id = metadata["conversation_id"];
      const char* agent_output_format = metadata["agent_output_audio_format"];
      const char* user_input_format = metadata["user_input_audio_format"];
      
      ESP_LOGD(TAG, "PARSE_JSON_BUF: conversation_id=%s", conversation_id ? conversation_id : "NULL");
      ESP_LOGD(TAG, "PARSE_JSON_BUF: agent_output_format=%s", agent_output_format ? agent_output_format : "NULL");
      ESP_LOGD(TAG, "PARSE_JSON_BUF: user_input_format=%s", user_input_format ? user_input_format : "NULL");
      
      if (conversation_id) { //we don't listen right now temporarily - this has always been disabled, since we are only testing playback for the initial message right now.
        this->conversation_id_ = conversation_id;
        ESP_LOGI(TAG, "PARSE_JSON_BUF: Conversation initiated: %s", conversation_id);
        
        // Store audio formats
        if (agent_output_format) {
          this->agent_output_audio_format_ = agent_output_format;
          ESP_LOGD(TAG, "PARSE_JSON_BUF: Agent output format: %s", agent_output_format);
        }
        if (user_input_format) {
          this->user_input_audio_format_ = user_input_format;
          ESP_LOGD(TAG, "PARSE_JSON_BUF: User input format: %s", user_input_format);
        }
        
        // Start listening
        ESP_LOGD(TAG, "PARSE_JSON_BUF: Setting state to LISTENING");
        this->set_state(StreamState::LISTENING);
        
        // Start microphone if available
        if (this->microphone_ && !this->microphone_->is_running()) {
          ESP_LOGD(TAG, "PARSE_JSON_BUF: Starting microphone capture for listening");
          this->microphone_->start();
          ESP_LOGD(TAG, "PARSE_JSON_BUF: Microphone started");
        } else {
          ESP_LOGD(TAG, "PARSE_JSON_BUF: Microphone not available or already running (mic=%p, running=%s)", 
                   this->microphone_, 
                   this->microphone_ && this->microphone_->is_running() ? "YES" : "NO");
        }
        
        ESP_LOGD(TAG, "PARSE_JSON_BUF: Triggering start events (%zu triggers)", this->on_start_triggers_.size());
        for (auto *trigger : this->on_start_triggers_) {
          ESP_LOGD(TAG, "PARSE_JSON_BUF: Triggering start event at %p", trigger);
          trigger->trigger();
        }
        ESP_LOGD(TAG, "PARSE_JSON_BUF: Triggering listening events (%zu triggers)", this->on_listening_triggers_.size());
        for (auto *trigger : this->on_listening_triggers_) {
          ESP_LOGD(TAG, "PARSE_JSON_BUF: Triggering listening event at %p", trigger);
          trigger->trigger();
        }
      } else {
        ESP_LOGW(TAG, "PARSE_JSON_BUF: No conversation_id in metadata");
      }
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No conversation_initiation_metadata_event found");
    }
    return;
  }
  
  // Handle audio events (corrected type name)
  if (strcmp(type, "audio") == 0) {
    ESP_LOGI(TAG, "PARSE_JSON_BUF: Processing audio event");
    JsonObject audio = root["audio_event"];
    if (audio) {
      ESP_LOGI(TAG, "PARSE_JSON_BUF: Found audio_event");
      const char* audio_base64 = audio["audio_base_64"];
      uint32_t event_id = audio["event_id"] | 0;
      
      ESP_LOGI(TAG, "PARSE_JSON_BUF: audio_base64=%s, event_id=%d", 
               audio_base64 ? "PRESENT" : "NULL", event_id);
      
      if (audio_base64) {
        size_t base64_len = strlen(audio_base64);
        ESP_LOGD(TAG, "PARSE_JSON_BUF: Received audio data (base64 length: %zu, event_id: %d)", base64_len, event_id);
        
        // Update timing for state management
        this->last_audio_response_time_ = millis();
        ESP_LOGD(TAG, "PARSE_JSON_BUF: Updated last_audio_response_time to %d", this->last_audio_response_time_);
        
        // Process audio chunks immediately for better real-time performance
        // Skip empty or very small chunks
        if (base64_len > 4) {
          ESP_LOGI(TAG, "PARSE_JSON_BUF: About to decode base64 audio data (length=%zu)...", base64_len);
          // Decode base64 audio data and play it immediately
          bool decode_success = this->decode_and_play_base64_audio(audio_base64);
          if (decode_success) {
            ESP_LOGI(TAG, "PARSE_JSON_BUF: Successfully decoded and played audio data");
            
            // Agent is speaking - change state
            ESP_LOGD(TAG, "PARSE_JSON_BUF: Setting state to SPEAKING");
            this->set_state(StreamState::SPEAKING);
            
            ESP_LOGD(TAG, "PARSE_JSON_BUF: Triggering speaking events (%zu triggers)", this->on_speaking_triggers_.size());
            for (auto *trigger : this->on_speaking_triggers_) {
              ESP_LOGD(TAG, "PARSE_JSON_BUF: Triggering speaking event at %p", trigger);
              trigger->trigger();
            }
          } else {
            ESP_LOGW(TAG, "PARSE_JSON_BUF: Failed to decode audio data");
          }
        } else {
          ESP_LOGD(TAG, "PARSE_JSON_BUF: Skipping small audio chunk (length=%zu)", base64_len);
        }
      } else {
        ESP_LOGW(TAG, "PARSE_JSON_BUF: No audio_base_64 in audio event");
      }
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No audio_event found in audio message");
    }
    return;
  }
  
  // Handle user transcript
  if (strcmp(type, "user_transcript") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing user_transcript");
    JsonObject transcript = root["user_transcription_event"];
    if (transcript) {
      const char* user_transcript = transcript["user_transcript"];
      if (user_transcript) {
        ESP_LOGI(TAG, "PARSE_JSON_BUF: User transcript: '%s'", user_transcript);
        // Could trigger an event here for transcript handling
      } else {
        ESP_LOGW(TAG, "PARSE_JSON_BUF: No user_transcript in user_transcription_event");
      }
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No user_transcription_event found");
    }
    return;
  }
  
  // Handle agent response
  if (strcmp(type, "agent_response") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing agent_response");
    JsonObject response = root["agent_response_event"];
    if (response) {
      const char* agent_response = response["agent_response"];
      if (agent_response) {
        ESP_LOGI(TAG, "PARSE_JSON_BUF: Agent response: '%s'", agent_response);
        // Could trigger an event here for response handling
      } else {
        ESP_LOGW(TAG, "PARSE_JSON_BUF: No agent_response in agent_response_event");
      }
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No agent_response_event found");
    }
    return;
  }
  
  // Handle internal tentative agent response
  if (strcmp(type, "internal_tentative_agent_response") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing internal_tentative_agent_response");
    JsonObject tentative = root["tentative_agent_response_internal_event"];
    if (tentative) {
      const char* tentative_response = tentative["tentative_agent_response"];
      if (tentative_response) {
        ESP_LOGD(TAG, "PARSE_JSON_BUF: Tentative agent response: '%s'", tentative_response);
      } else {
        ESP_LOGW(TAG, "PARSE_JSON_BUF: No tentative_agent_response in event");
      }
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No tentative_agent_response_internal_event found");
    }
    return;
  }
  
  // Handle VAD score
  if (strcmp(type, "vad_score") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing vad_score");
    JsonObject vad = root["vad_score_event"];
    if (vad) {
      float vad_score = vad["vad_score"] | 0.0f;
      ESP_LOGD(TAG, "PARSE_JSON_BUF: VAD score: %.2f", vad_score);
      // Could use this for voice activity detection
    } else {
      ESP_LOGD(TAG, "PARSE_JSON_BUF: No vad_score_event found");
    }
    return;
  }
  
  // Handle interruption
  if (strcmp(type, "interruption") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing interruption event");
    JsonObject interruption = root["interruption_event"];
    if (interruption) {
      ESP_LOGD(TAG, "PARSE_JSON_BUF: Interruption event received");
      // Handle interruption logic - could stop current audio playback
      if (this->speaker_) {
        ESP_LOGD(TAG, "PARSE_JSON_BUF: Stopping speaker due to interruption");
        this->speaker_->stop();
      }
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No interruption_event found");
    }
    return;
  }
  
  // Handle agent_response_correction
  if (strcmp(type, "agent_response_correction") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing agent_response_correction");
    JsonObject correction = root["agent_response_correction_event"];
    if (correction) {
      const char* corrected_text = correction["corrected_text"];
      if (corrected_text) {
        ESP_LOGD(TAG, "PARSE_JSON_BUF: Agent response correction: '%s'", corrected_text);
      } else {
        ESP_LOGW(TAG, "PARSE_JSON_BUF: No corrected_text in correction event");
      }
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No agent_response_correction_event found");
    }
    return;
  }
  
  // Handle ping with proper response
  if (strcmp(type, "ping") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing ping");
    JsonObject ping = root["ping_event"];
    if (ping) {
      uint32_t event_id = ping["event_id"] | 0;
      uint32_t ping_ms = ping["ping_ms"] | 0;
      
      ESP_LOGD(TAG, "PARSE_JSON_BUF: Ping received: event_id=%d, ping_ms=%d", event_id, ping_ms);
      
      // Send pong response with event_id
      ESP_LOGD(TAG, "PARSE_JSON_BUF: Sending pong response...");
      std::string pong_message = json::build_json([event_id](JsonObject root) {
        root["type"] = "pong";
        root["event_id"] = event_id;
      });
      this->send_websocket_message(pong_message);
      ESP_LOGD(TAG, "PARSE_JSON_BUF: Pong sent");
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No ping_event found");
    }
    return;
  }
  
  // Handle client tool call
  if (strcmp(type, "client_tool_call") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing client_tool_call");
    JsonObject tool_call = root["client_tool_call"];
    if (tool_call) {
      const char* tool_name = tool_call["tool_name"];
      const char* tool_call_id = tool_call["tool_call_id"];
      JsonObject parameters = tool_call["parameters"];
      
      if (tool_name && tool_call_id) {
        ESP_LOGI(TAG, "PARSE_JSON_BUF: Client tool call: %s (id: %s)", tool_name, tool_call_id);
        // Handle tool calls - would need to implement tool handling
      } else {
        ESP_LOGW(TAG, "PARSE_JSON_BUF: Missing tool_name or tool_call_id in client_tool_call");
      }
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No client_tool_call object found");
    }
    return;
  }
  
  // Handle contextual update
  if (strcmp(type, "contextual_update") == 0) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Processing contextual_update");
    const char* text = root["text"];
    if (text) {
      ESP_LOGD(TAG, "PARSE_JSON_BUF: Contextual update: '%s'", text);
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No text in contextual_update");
    }
    return;
  }
  
  // Log unknown message types for debugging
  ESP_LOGW(TAG, "PARSE_JSON_BUF: Unknown message type: '%s'", type);
  ESP_LOGD(TAG, "PARSE_JSON_BUF: Available fields in unknown message:");
  for (JsonPair kv : root) {
    ESP_LOGD(TAG, "PARSE_JSON_BUF:   - %s", kv.key().c_str());
  }
}

void ElevenLabsStream::handle_websocket_binary(const uint8_t *data, size_t length) {
  ESP_LOGD(TAG, "HANDLE_WS_BIN: Received binary WebSocket data");
  ESP_LOGD(TAG, "HANDLE_WS_BIN: Data pointer=%p, length=%zu", data, length);
  ESP_LOGD(TAG, "HANDLE_WS_BIN: Received binary data: %d bytes", length);
  
  // ElevenLabs API uses JSON with base64 encoded audio, not binary frames
  // This method is kept for completeness but may not be used by ElevenLabs
  ESP_LOGW(TAG, "HANDLE_WS_BIN: Binary WebSocket frames not expected in ElevenLabs protocol");
  ESP_LOGD(TAG, "HANDLE_WS_BIN: Binary data handling complete");
}

void ElevenLabsStream::handle_audio_response(const uint8_t *data, size_t length) {
  ESP_LOGI(TAG, "HANDLE_AUDIO: Processing audio response");
  ESP_LOGI(TAG, "HANDLE_AUDIO: Data pointer=%p, length=%zu", data, length);
  ESP_LOGI(TAG, "HANDLE_AUDIO: Playing audio response: %d bytes", length);
  
  if (!data || length == 0) {
    ESP_LOGD(TAG, "HANDLE_AUDIO: No audio data to play (data=%p, length=%zu)", data, length);
    return;
  }
  
  if (!this->speaker_) {
    ESP_LOGD(TAG, "HANDLE_AUDIO: No speaker configured");
    return;
  }
  
  ESP_LOGD(TAG, "HANDLE_AUDIO: Speaker available at %p", this->speaker_);
  ESP_LOGD(TAG, "HANDLE_AUDIO: Speaker running=%s", this->speaker_->is_running() ? "YES" : "NO");
  
  // Parse sample rate from agent_output_audio_format (e.g., "pcm_44100")
  uint32_t sample_rate = 44100; // Default to 44.1kHz
  if (!this->agent_output_audio_format_.empty()) {
    // Extract sample rate from format string like "pcm_44100"
    size_t underscore_pos = this->agent_output_audio_format_.find('_');
    if (underscore_pos != std::string::npos) {
      std::string rate_str = this->agent_output_audio_format_.substr(underscore_pos + 1);
      sample_rate = std::stoul(rate_str);
      ESP_LOGI(TAG, "HANDLE_AUDIO: Parsed sample rate: %d Hz from format '%s'", sample_rate, this->agent_output_audio_format_.c_str());
    }
  }
  
  // Set the input audio stream info for the resampler based on ElevenLabs format
  // ElevenLabs sends 16-bit mono PCM audio
  esphome::audio::AudioStreamInfo input_stream_info(16, 1, sample_rate); // 16-bit, mono, parsed sample rate
  ESP_LOGI(TAG, "HANDLE_AUDIO: Setting input audio stream info: %d-bit, %d channels, %d Hz", 
           16, 1, sample_rate);
  
  // Configure the speaker with the correct input format BEFORE playing any audio
  this->speaker_->set_audio_stream_info(input_stream_info);
  ESP_LOGI(TAG, "HANDLE_AUDIO: Input audio stream info set on speaker - letting speaker start naturally with play()");
  
  // Ensure the speaker is started before sending audio data
  if (!this->speaker_->is_running()) {
    ESP_LOGI(TAG, "HANDLE_AUDIO: Starting resampler speaker before sending audio data");
    this->speaker_->start();
    // Give the speaker time to start up properly and initialize its pipeline
    // The resampler needs to connect to mixer which connects to I2S hardware
    vTaskDelay(pdMS_TO_TICKS(100)); // Increased startup delay for pipeline initialization
  }
  
  ESP_LOGI(TAG, "HANDLE_AUDIO: Agent output format: %s", this->agent_output_audio_format_.c_str());
  
  // Let's examine the first few bytes to understand the audio format
  if (length >= 8) {
    ESP_LOGI(TAG, "HANDLE_AUDIO: First 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x", 
             data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
  }
  
  size_t samples = length / 2; // 16-bit samples = 2 bytes each
  float duration_ms = (float)samples / (sample_rate / 1000.0f);
  ESP_LOGI(TAG, "HANDLE_AUDIO: Estimated audio duration: %.1f ms (%zu samples at %d Hz)", duration_ms, samples, sample_rate);
  
  // Send audio data to resampler - now with correct input format set
  ESP_LOGI(TAG, "HANDLE_AUDIO: Sending audio data to resampler with input format %d Hz -> output 48 kHz", sample_rate);
  
  // Send the audio data to the speaker (resampler pipeline)
  // The resampler will convert 44.1kHz->48kHz, then send to mixer, then to I2S hardware
  size_t bytes_written = this->speaker_->play(data, length);
  
  if (bytes_written != length) {
    ESP_LOGW(TAG, "HANDLE_AUDIO: Speaker play() returned %zu bytes, expected %zu bytes", bytes_written, length);
  } else {
    ESP_LOGI(TAG, "HANDLE_AUDIO: Successfully sent %zu bytes to speaker pipeline", bytes_written);
  }
  
  // Don't block here - let the audio play naturally through the pipeline
  // The resampler and mixer will handle the timing and conversion
  ESP_LOGI(TAG, "HANDLE_AUDIO: Audio data sent to resampler pipeline, continuing...");
  
  // Add debug info about speaker state after sending audio
  if (this->speaker_ && this->speaker_->is_running()) {
    ESP_LOGI(TAG, "HANDLE_AUDIO: Resampler speaker is running successfully");
  } else if (this->speaker_) {
    ESP_LOGW(TAG, "HANDLE_AUDIO: Resampler speaker NOT running - audio may not play");
  }
  
  ESP_LOGI(TAG, "HANDLE_AUDIO: Audio response handling complete (%zu total bytes)", length);
}

void ElevenLabsStream::handle_error(const std::string &error_message) {
  ESP_LOGE(TAG, "=== ERROR HANDLER CALLED ===");
  ESP_LOGE(TAG, "ERROR: %s", error_message.c_str());
  ESP_LOGD(TAG, "ERROR: Current state=%d", static_cast<int>(this->state_));
  ESP_LOGD(TAG, "ERROR: WebSocket connected=%s", this->websocket_connected_ ? "YES" : "NO");
  ESP_LOGD(TAG, "ERROR: Setting state to ERROR");
  
  // Invalidate signed URL on connection errors - it might be expired
  if (error_message.find("Failed to") != std::string::npos || 
      error_message.find("timeout") != std::string::npos ||
      error_message.find("connection") != std::string::npos) {
    ESP_LOGW(TAG, "ERROR: Connection-related error detected, invalidating signed URL");
    this->signed_url_valid_ = false;
    this->signed_url_.clear();
  }
  
  this->set_state(StreamState::ERROR);
  
  ESP_LOGD(TAG, "ERROR: Triggering error events (%zu triggers)", this->on_error_triggers_.size());
  for (auto *trigger : this->on_error_triggers_) {
    ESP_LOGD(TAG, "ERROR: Triggering error event at %p with message: '%s'", trigger, error_message.c_str());
    trigger->trigger(error_message);
  }
  ESP_LOGD(TAG, "ERROR: All error events triggered");
  ESP_LOGE(TAG, "=== ERROR HANDLER COMPLETE ===");
}

void ElevenLabsStream::send_conversation_init() {
  ESP_LOGD(TAG, "SEND_CONV_INIT: Sending conversation initialization");
  
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
  
  ESP_LOGD(TAG, "SEND_CONV_INIT: Sending conversation init: %s", message.c_str());
  this->send_websocket_message(message);
  ESP_LOGD(TAG, "SEND_CONV_INIT: Conversation init sent");
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
  
  this->send_websocket_message(message);
}

void ElevenLabsStream::send_audio_chunk(const std::vector<int16_t> &audio_data) {
  if (!this->websocket_connected_ || !this->websocket_client_ || audio_data.empty()) {
    ESP_LOGD(TAG, "SEND_AUDIO: Cannot send audio - conditions not met");
    return;
  }
  
  // Convert audio data to bytes
  const uint8_t* audio_bytes = reinterpret_cast<const uint8_t*>(audio_data.data());
  size_t audio_size = audio_data.size() * sizeof(int16_t);
  
  // Encode audio as base64 for WebSocket transmission
  std::string audio_base64 = base64_encode(audio_bytes, audio_size);
  
  if (audio_base64.empty()) {
    ESP_LOGE(TAG, "SEND_AUDIO: Failed to encode audio data to base64");
    return;
  }
  // Send as user_audio_chunk according to protocol
  std::string message = json::build_json([&audio_base64](JsonObject root) {
    root["user_audio_chunk"] = audio_base64;
  });
  
  ESP_LOGD(TAG, "SEND_AUDIO: Sending audio chunk: %d samples, %d bytes, base64 length: %d", 
           audio_data.size(), audio_size, audio_base64.length());
  
  this->send_websocket_message(message);
}

void ElevenLabsStream::handle_microphone_data(const std::vector<uint8_t> &data) {
  if (this->state_ != StreamState::LISTENING || !this->websocket_connected_ || data.empty()) {
    ESP_LOGD(TAG, "HANDLE_MIC: Skipping microphone data - conditions not met");
    ESP_LOGD(TAG, "HANDLE_MIC:   state=%d (LISTENING=%d), connected=%s, empty=%s",
             static_cast<int>(this->state_), static_cast<int>(StreamState::LISTENING),
             this->websocket_connected_ ? "NO" : "YES",
             data.empty() ? "YES" : "NO");
    return;
  }
  
  // Ensure data size is even (each sample is 2 bytes)
  if (data.size() % 2 != 0) {
    ESP_LOGW(TAG, "HANDLE_MIC: Received odd number of bytes for int16_t samples: %d", data.size());
    return;
  }
  
  // Convert uint8_t data to int16_t samples
  std::vector<int16_t> audio_samples;
  audio_samples.resize(data.size() / sizeof(int16_t));
  
  memcpy(audio_samples.data(), data.data(), data.size());
  
  // Send audio chunk to ElevenLabs
  this->send_audio_chunk(audio_samples);
}

// WebSocket event handler
void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
  ElevenLabsStream *stream = static_cast<ElevenLabsStream*>(handler_args);
  esp_websocket_event_data_t *data = static_cast<esp_websocket_event_data_t*>(event_data);
  
  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      ESP_LOGI(TAG, "WS_EVENT: WEBSOCKET_EVENT_CONNECTED");
      ESP_LOGD(TAG, "WS_EVENT: Setting websocket_connected_ = true");
      stream->websocket_connected_ = true;
      ESP_LOGD(TAG, "WS_EVENT: Setting state to CONNECTED");
      stream->set_state(StreamState::CONNECTED);
      
      // Send initial conversation setup
      ESP_LOGD(TAG, "WS_EVENT: Sending conversation initialization...");
      stream->send_conversation_init();
      
      // Trigger connected events
      ESP_LOGD(TAG, "WS_EVENT: Triggering connected events (%zu triggers)", stream->on_connected_triggers_.size());
      for (auto *trigger : stream->on_connected_triggers_) {
        ESP_LOGD(TAG, "WS_EVENT: Triggering connected event at %p", trigger);
        trigger->trigger();
      }
      ESP_LOGD(TAG, "WS_EVENT: CONNECTED event handling complete");
      break;
      
    case WEBSOCKET_EVENT_DISCONNECTED:
      ESP_LOGI(TAG, "WS_EVENT: WEBSOCKET_EVENT_DISCONNECTED");
      ESP_LOGD(TAG, "WS_EVENT: Setting websocket_connected_ = false");
      stream->websocket_connected_ = false;
      ESP_LOGD(TAG, "WS_EVENT: Setting state to IDLE");
      stream->set_state(StreamState::IDLE);
      
      ESP_LOGD(TAG, "WS_EVENT: Triggering disconnected events (%zu triggers)", stream->on_disconnected_triggers_.size());
      for (auto *trigger : stream->on_disconnected_triggers_) {
        ESP_LOGD(TAG, "WS_EVENT: Triggering disconnected event at %p", trigger);
        trigger->trigger();
      }
      ESP_LOGD(TAG, "WS_EVENT: DISCONNECTED event handling complete");
      break;
      
    case WEBSOCKET_EVENT_DATA:
      if (data->op_code == 0x08) { // Close frame
        ESP_LOGW(TAG, "WS_EVENT: WebSocket close frame received");
        if (data->data_len >= 2) {
          uint16_t close_code = (data->data_ptr[0] << 8) | data->data_ptr[1];
          ESP_LOGW(TAG, "WS_EVENT: Close frame with code=%d", close_code);
        } else {
          ESP_LOGW(TAG, "WS_EVENT: Close frame without code");
        }
        // Handle close frame - this will trigger WEBSOCKET_EVENT_DISCONNECTED
        break;
      }
      
      if (data->op_code == 0x01) { // Text frame
        // Handle text messages with robust fragmentation support using reassembler
        if (data->data_len > 0) {
          
          // Use the reassembler to handle fragmentation
          bool complete = stream->reassembler_.add(data);
          
          if (complete) {
            ESP_LOGD(TAG, "WS_EVENT: Complete message assembled, processing...");
            
            // Get pointer to complete message in reassembler buffer
            const uint8_t* buffer_ptr = stream->reassembler_.getBuffer();
            size_t buffer_size = stream->reassembler_.getSize();
            
            if (buffer_ptr && buffer_size > 0) {
              stream->handle_websocket_message(buffer_ptr, buffer_size);
              
              // Reset the reassembler after processing
              stream->reassembler_.reset();
            }
          }
        } else {
          ESP_LOGW(TAG, "WS_EVENT: Text frame with no data");
        }
      } else if (data->op_code == 0x02) { // Binary frame
        ESP_LOGD(TAG, "WS_EVENT: Binary frame received: %d bytes", data->data_len);
        if (data->data_len > 0) {
          ESP_LOGD(TAG, "WS_EVENT: Processing binary data...");
          stream->handle_websocket_binary((const uint8_t*)data->data_ptr, data->data_len);
          ESP_LOGD(TAG, "WS_EVENT: Binary data processing complete");
        }
      } else {
        ESP_LOGW(TAG, "WS_EVENT: Unsupported WebSocket opcode: 0x%02x", data->op_code);
      }
      break;
      
    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "WS_EVENT: WEBSOCKET_EVENT_ERROR");
      ESP_LOGD(TAG, "WS_EVENT: Handling WebSocket error...");
      stream->handle_error("WebSocket connection error");
      ESP_LOGD(TAG, "WS_EVENT: Error handling complete");
      break;
      
    default:
      ESP_LOGD(TAG, "WS_EVENT: Unknown WebSocket event: %d", event_id);
      break;
  }
}

}  // namespace elevenlabs_stream
}  // namespace esphome
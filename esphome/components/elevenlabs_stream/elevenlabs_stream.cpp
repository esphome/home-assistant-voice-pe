// Disconnects from ElevenLabs and resets protocol state.
// See ElevenLabs API docs: https://docs.elevenlabs.io/api-reference/convai
#include "elevenlabs_stream.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/audio/audio.h"

#include "elevenlabs_client.h"
#include "json.h"
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>

namespace esphome {
namespace elevenlabs_stream {

using namespace esphome::json;

static const char* TAG = "elevenlabs_stream";

// Helper to convert StreamState enum to string
static const char* stream_state_to_string(StreamState state) {
  switch (state) {
    case StreamState::OFF: return "OFF";
    case StreamState::ON: return "ON";
    default: return "UNKNOWN";
  }
}

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


void ElevenLabsStream::handle_websocket_disconnected() {
  ESP_LOGI(TAG, "WS_EVENT: WEBSOCKET_EVENT_DISCONNECTED");
  this->set_state(StreamState::OFF);
  ESP_LOGD(TAG, "WS_EVENT: Triggering end events (%zu triggers)", this->on_end_triggers_.size());
  for (auto *trigger : this->on_end_triggers_) {
    ESP_LOGD(TAG, "WS_EVENT: Triggering end event at %p", trigger);
    trigger->trigger();
  }
  ESP_LOGD(TAG, "WS_EVENT: DISCONNECTED event handling complete");
}

bool ElevenLabsStream::decode_and_play_base64_audio(const char* base64_data) {
  size_t input_len = strlen(base64_data);
  if (!base64_data || input_len == 0) {
    ESP_LOGW(TAG, "DECODE_B64: No base64 data provided");
    return false;
  }

  // Two-step base64 decode: first get required output length
  size_t required_output_len = 0;
  int ret = mbedtls_base64_decode(nullptr, 0, &required_output_len, (const unsigned char*)base64_data, input_len);
  if (ret != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
    ESP_LOGE(TAG, "DECODE_B64: Failed to get base64 output length: %d", ret);
    return false;
  }

  // Allocate temporary buffer in PSRAM
  uint8_t* temp_audio_buffer = (uint8_t*)heap_caps_malloc(required_output_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!temp_audio_buffer) {
    ESP_LOGE(TAG, "DECODE_B64: Failed to allocate temporary buffer in PSRAM");
    return false;
  }

  size_t output_len = 0;
  ret = mbedtls_base64_decode(temp_audio_buffer, required_output_len, &output_len, (const unsigned char*)base64_data, input_len);
  if (ret != 0) {
    ESP_LOGE(TAG, "DECODE_B64: Failed to decode base64 audio data: %d", ret);
    heap_caps_free(temp_audio_buffer);
    return false;
  }

  ESP_LOGD(TAG, "DECODE_B64: Decoded %zu bytes of audio (PSRAM)", output_len);

  size_t bytes_written = 0;
  for (int retry=0; retry<5 && bytes_written==0; ++retry) {
    bytes_written = speaker_->play(temp_audio_buffer, output_len);
    if (bytes_written==0) delay(5);
  }

  // Playback: play decoded audio directly
  ESP_LOGD(TAG, "DECODE_B64: Played %zu bytes from temp buffer", bytes_written);

  if (bytes_written != output_len) {
    ESP_LOGE(TAG, "DECODE_B64: Played bytes mismatch: expected %zu, got %zu", output_len, bytes_written);
    heap_caps_free(temp_audio_buffer);
    return false;
  }

  // Free temporary buffer
  heap_caps_free(temp_audio_buffer);

  return true;
}

void ElevenLabsStream::setup() {
  ESP_LOGCONFIG(TAG, "=== SETUP START ===");
  ESP_LOGCONFIG(TAG, "Setting up ElevenLabs Stream...");
  ESP_LOGD(TAG, "SETUP: Component instance created at %p", this);
  ESP_LOGD(TAG, "SETUP: Agent ID: '%s'", this->agent_id_.c_str());
  ESP_LOGD(TAG, "SETUP: API Key configured: %s", this->api_key_.empty() ? "NO" : "YES");
  
  if (this->agent_id_.empty()) {
    ESP_LOGE(TAG, "SETUP: Agent ID not configured - SETUP FAILED");
    this->mark_failed();
    return;
  }

  if (!this->client_) {
    this->client_ = new ElevenLabsClient(this->agent_id_, this->api_key_);
  }

  this->speaker_->add_audio_output_callback([this](uint32_t _a, int64_t _b) {
    this->cancel_timeout("audio_output_callback");
    this->set_timeout("audio_output_callback", 100, [this]() {
      if(this->microphone_->is_running() && this->speaker_is_active_) {
        for (auto *trigger : this->on_listening_triggers_) {
            trigger->trigger();
        }
      }

      ESP_LOGD(TAG, "DECODE_B64: speaker finished");
      this->speaker_is_active_ = false;
    });
  });
  
  ESP_LOGD(TAG, "SETUP: Initial state set to %d (IDLE)", static_cast<int>(this->state_));
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
    ESP_LOGV(TAG, "LOOP: Watchdog fed at loop count %d, state=%s", 
             loop_count, this->state_ == StreamState::OFF ? "OFF" : "ON");
  }
  
  // Send periodic heartbeat when connected
  if (this->client_ && this->state_ == StreamState::ON) {
    uint32_t heartbeat_elapsed = millis() - this->last_heartbeat_;
    if (heartbeat_elapsed > 10000) {  // 10 seconds instead of 20
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
  ESP_LOGD(TAG, "START_STREAM: Current state=%s", stream_state_to_string(this->state_));
  ESP_LOGD(TAG, "START_STREAM: Agent ID='%s'", this->agent_id_.c_str());
  ESP_LOGD(TAG, "START_STREAM: Microphone=%p, Speaker=%p", this->microphone_, this->speaker_);
  
  if (this->state_ == StreamState::ON) {
    ESP_LOGW(TAG, "START_STREAM: Cannot start stream - already ON");
    return false;
  }

  ESP_LOGD(TAG, "SET_STATE: Triggering replying events (%zu triggers)", this->on_replying_triggers_.size());
  for (auto *trigger : this->on_replying_triggers_) {
    trigger->trigger();
  }

  ESP_LOGI(TAG, "START_STREAM: Starting ElevenLabs stream...");
  this->connection_start_time_ = millis();
  ESP_LOGD(TAG, "START_STREAM: Connection start time set to %d", this->connection_start_time_);

  ESP_LOGD(TAG, "START_STREAM: Connecting to ElevenLabs...");
  bool connected = this->client_->connect(
    this->signed_url_,
    [this](const uint8_t* buffer, size_t length) { this->handle_websocket_message(buffer, length); },
    [this]() { 
      this->set_state(StreamState::ON); 
      this->send_conversation_init();
    },
    [this]() { this->handle_websocket_disconnected(); },
    [this](const std::string& err) { this->handle_error(err); }
  );
  if (!connected) {
    ESP_LOGE(TAG, "START_STREAM: Failed to connect to ElevenLabs WebSocket");
    this->handle_error("Failed to connect to ElevenLabs WebSocket");
    return false;
  }
  ESP_LOGD(TAG, "=== START_STREAM COMPLETE ===");
  return true;
}

void ElevenLabsStream::stop_stream() {
  ESP_LOGI(TAG, "=== STOP_STREAM CALLED ===");
  ESP_LOGI(TAG, "STOP_STREAM: Stopping ElevenLabs stream...");
  if (this->microphone_ && this->microphone_->is_running()) {
    ESP_LOGD(TAG, "STOP_STREAM: Stopping microphone capture");
    this->microphone_->stop();
    ESP_LOGD(TAG, "STOP_STREAM: Microphone stopped");
  }
  this->speaker_is_active_ = false;
  this->speaker_start_time_ = 0;
  this->speaker_end_time_ = 0;
  this->accumulated_duration_ms_ = 0;
  ESP_LOGD(TAG, "STOP_STREAM: Speaker activity tracking reset");
  if (this->client_) {
    this->client_->disconnect();
  }
  this->set_state(StreamState::OFF);
  ESP_LOGD(TAG, "STOP_STREAM: Triggering end events (%zu triggers)", this->on_end_triggers_.size());
  for (auto *trigger : this->on_end_triggers_) {
    trigger->trigger();
  }
  ESP_LOGD(TAG, "=== STOP_STREAM COMPLETE ===");
}

void ElevenLabsStream::renew_signed_url_if_needed() {
  if (!this->client_) {
    return;
  }
  uint32_t current_time = millis();
  bool should_renew = false;
  if (this->signed_url_.empty()) {
    ESP_LOGD(TAG, "RENEW: No signed URL available, will renew");
    should_renew = true;
  } else if (current_time - this->last_signed_url_renewal_ >= this->signed_url_renewal_interval_) {
    uint32_t elapsed_minutes = (current_time - this->last_signed_url_renewal_) / 60000;
    ESP_LOGI(TAG, "RENEW: Signed URL renewal interval reached (%d minutes elapsed)", elapsed_minutes);
    should_renew = true;
  }
  if (should_renew) {
    ESP_LOGI(TAG, "RENEW: Renewing signed URL for fast connections...");
    if (this->state_ == StreamState::OFF) {
      std::string signed_url;
      if (this->client_->get_signed_url(signed_url)) {
        this->last_signed_url_renewal_ = current_time;
        this->signed_url_ = signed_url;
        ESP_LOGI(TAG, "RENEW: Signed URL renewed successfully");
      } else {
        ESP_LOGW(TAG, "RENEW: Failed to renew signed URL");
        this->signed_url_.clear();
      }
    } else {
      ESP_LOGD(TAG, "RENEW: Deferring renewal - stream is active (state=ON)");
    }
  }
}

void ElevenLabsStream::send_websocket_message(const std::string &message) {
  if (!this->client_ || !this->client_->is_connected() || message.empty()) {
    ESP_LOGW(TAG, "SEND_WS_MSG: Cannot send message - WebSocket not connected or message empty");
    return;
  }
  if (!this->client_->send_message(message)) {
    ESP_LOGE(TAG, "SEND_WS_MSG: Failed to send WebSocket message");
  }
}

void ElevenLabsStream::handle_websocket_message(const uint8_t *buffer, size_t length) {
  if (!buffer || length == 0) {
    ESP_LOGW(TAG, "HANDLE_WS_MSG: Received empty WebSocket message");
    return;
  }
  this->parse_json_message_from_buffer(buffer, length);
  ESP_LOGV(TAG, "HANDLE_WS_MSG: Message processing complete");
}

void ElevenLabsStream::parse_json_message_from_buffer(const uint8_t *buffer, size_t length) {
  // Use new JsonDeserializer class
  auto json_doc = JsonDeserializer::parse(buffer, length);
  if (!json_doc) {
    ESP_LOGE(TAG, "PARSE_JSON_BUF: Failed to parse JSON buffer");
    return;
  }
  JsonObject root = json_doc->as<JsonObject>();
  const char* type = root["type"];
  if (!type) {
    ESP_LOGW(TAG, "PARSE_JSON_BUF: Message missing type field");
    ESP_LOGD(TAG, "PARSE_JSON_BUF: Available root fields:");
    for (JsonPair kv : root) {
      ESP_LOGD(TAG, "PARSE_JSON_BUF:   - %s", kv.key().c_str());
    }
    return;
  }
  
  ESP_LOGV(TAG, "PARSE_JSON_BUF: Message type: '%s'", type);
  
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
        
        // Initialize speaker with correct audio format early for faster response
        if (this->speaker_ && agent_output_format) {
          // Parse sample rate from agent_output_audio_format (e.g., "pcm_44100")
          uint32_t sample_rate = 44100; // Default to 44.1kHz
          size_t underscore_pos = this->agent_output_audio_format_.find('_');
          if (underscore_pos != std::string::npos) {
            std::string rate_str = this->agent_output_audio_format_.substr(underscore_pos + 1);
            sample_rate = std::stoul(rate_str);
            ESP_LOGI(TAG, "PARSE_JSON_BUF: Parsed sample rate: %d Hz from format '%s'", sample_rate, this->agent_output_audio_format_.c_str());
          }
          
          // Set the input audio stream info for the resampler based on ElevenLabs format
          esphome::audio::AudioStreamInfo input_stream_info(16, 1, sample_rate); // 16-bit, mono, parsed sample rate
          ESP_LOGI(TAG, "PARSE_JSON_BUF: Setting input audio stream info: %d-bit, %d channels, %d Hz", 
                   16, 1, sample_rate);
          
          // Configure the speaker with the correct input format
          if (!this->initial_audio_stream_info_set_) {
            this->initial_audio_stream_info_ = this->speaker_->get_audio_stream_info();
            this->initial_audio_stream_info_set_ = true;
          }
          this->speaker_->set_audio_stream_info(input_stream_info);
          ESP_LOGI(TAG, "PARSE_JSON_BUF: Audio stream info configured on speaker for faster playback");
          
          // Start the speaker early for immediate readiness
          ESP_LOGI(TAG, "PARSE_JSON_BUF: Starting speaker early for faster audio response");
          this->speaker_->start();
        }
        
        ESP_LOGD(TAG, "PARSE_JSON_BUF: Conversation initialized - already in ON state");
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
    JsonObject audio = root["audio_event"];
    if (audio) {
      const char* audio_base64 = audio["audio_base_64"];
      uint32_t event_id = audio["event_id"] | 0;
      
      if (audio_base64) {
        size_t base64_len = strlen(audio_base64);
        
        // Update timing for state management
        this->last_audio_response_time_ = millis();

        if(!this->speaker_is_active_) {
          this->speaker_is_active_ = true;
          
          for (auto *trigger : this->on_replying_triggers_) {
              trigger->trigger();
          }
        }
        
        // Decode base64 audio data and play it immediately
        bool decode_success = this->decode_and_play_base64_audio(audio_base64);
        if (!decode_success) {
          ESP_LOGW(TAG, "PARSE_JSON_BUF: Failed to decode audio data");
        }
      }
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
  
  // Handle VAD score
  if (strcmp(type, "vad_score") == 0) {
    JsonObject vad = root["vad_score_event"];
    if (vad) {
      float vad_score = vad["vad_score"] | 0.0f;
      if(vad_score <= 0.0f && this->speaker_is_active_) {
        return; // Skip invalid scores
      }

      float led_threshold = 0.25f;
      if(vad_score > led_threshold) {
        if(this->last_vad_score_ <= led_threshold) {
          for (auto *trigger : this->on_listening_triggers_) {
              trigger->trigger();
          }
        }
      } else {
        if(this->last_vad_score_ > led_threshold) {
          for (auto *trigger : this->on_processing_triggers_) {
              trigger->trigger();
          }
        }
      }

      this->last_vad_score_ = vad_score;

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
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No interruption_event found");
    }
    return;
  }
  
  // Handle ping with proper response
  if (strcmp(type, "ping") == 0) {
    JsonObject ping = root["ping_event"];
    if (ping) {
      uint32_t event_id = ping["event_id"] | 0;
      uint32_t ping_ms = ping["ping_ms"] | 0;
      
      // Send pong response with event_id
      std::string pong_message = json::build_json([event_id](JsonObject root) {
        root["type"] = "pong";
        root["event_id"] = event_id;
      });
      this->send_websocket_message(pong_message);
    } else {
      ESP_LOGW(TAG, "PARSE_JSON_BUF: No ping_event found");
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

// Handles errors, logs details, and triggers error automations.
void ElevenLabsStream::handle_error(const std::string &error_message) {
  ESP_LOGE(TAG, "=== ERROR HANDLER CALLED ===");
  ESP_LOGE(TAG, "ERROR: %s", error_message.c_str());
  ESP_LOGD(TAG, "ERROR: Setting state to OFF");
  
  // Invalidate signed URL on connection errors - it might be expired
  if (error_message.find("Failed to") != std::string::npos || 
      error_message.find("timeout") != std::string::npos ||
      error_message.find("connection") != std::string::npos) {
    ESP_LOGW(TAG, "ERROR: Connection-related error detected, invalidating signed URL");
    this->signed_url_.clear();
  }
  
  if (this->client_) {
    this->client_->disconnect();
  }
  this->set_state(StreamState::OFF);
  ESP_LOGD(TAG, "ERROR: Triggering error events (%zu triggers)", this->on_error_triggers_.size());
  for (auto *trigger : this->on_error_triggers_) {
    ESP_LOGD(TAG, "ERROR: Triggering error event at %p with message: '%s'", trigger, error_message.c_str());
    trigger->trigger(error_message);
  }
  ESP_LOGD(TAG, "ERROR: All error events triggered");
  ESP_LOGE(TAG, "=== ERROR HANDLER COMPLETE ===");
}

// Sends the initial conversation setup message to ElevenLabs.
// See ElevenLabs API docs: https://docs.elevenlabs.io/api-reference/convai
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
    
    // Language configuration
    agent["first_message"] = "";
  });
  
  ESP_LOGD(TAG, "SEND_CONV_INIT: Sending conversation init: %s", message.c_str());
  this->send_websocket_message(message);
  ESP_LOGD(TAG, "SEND_CONV_INIT: Conversation init sent");
}

// Sends a ping message to the ElevenLabs WebSocket for keepalive.
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

// Encodes and sends an audio chunk to ElevenLabs as base64.
void ElevenLabsStream::send_audio_chunk(const std::vector<int16_t> &audio_data) {
  if (!this->client_ || !this->client_->is_connected() || audio_data.empty()) {
    ESP_LOGD(TAG, "SEND_AUDIO: Cannot send audio - client connected=%s, data_empty=%s",
             this->client_ && this->client_->is_connected() ? "YES" : "NO",
             audio_data.empty() ? "YES" : "NO");
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
  this->send_websocket_message(message);
}

void ElevenLabsStream::handle_microphone_data(const std::vector<uint8_t> &data) {
  // Only process microphone data if stream is ON, websocket is connected, and data is present
  if (this->state_ != StreamState::ON || !this->client_ || !this->client_->is_connected() || data.empty()) {
    ESP_LOGV(TAG, "HANDLE_MIC: Skipping - state=%s, client connected=%s, data_empty=%s",
             this->state_ == StreamState::ON ? "ON" : "OFF",
             this->client_ && this->client_->is_connected() ? "YES" : "NO",
             data.empty() ? "YES" : "NO");
    return;
  }
  // Block microphone input if speaker is active or agent audio is playing
  if (this->speaker_is_active_) {
    ESP_LOGV(TAG, "HANDLE_MIC: Microphone blocked - speaker is active or agent audio playing");
    return;
  }

  size_t num_samples_32bit = data.size() / 4;
  const int32_t* samples_32bit = reinterpret_cast<const int32_t*>(data.data());
  std::vector<int16_t> audio_samples;
  audio_samples.reserve(num_samples_32bit);
  for (size_t i = 0; i < num_samples_32bit; i++) {
    int16_t sample16 = static_cast<int16_t>(samples_32bit[i] >> 16);
    audio_samples.push_back(sample16);
  }

  // Convert stereo to mono by averaging each left/right sample pair
  std::vector<int16_t> mono_samples;
  mono_samples.reserve(audio_samples.size() / 2);
  for (size_t i = 0; i + 1 < audio_samples.size(); i += 2) {
    int16_t left = audio_samples[i];
    int16_t right = audio_samples[i + 1];
    int16_t mono = (left + right) / 2;
    mono_samples.push_back(mono);
  }

  // Send mono buffer to ElevenLabs pipeline
  this->send_audio_chunk(mono_samples);
}

void ElevenLabsStream::set_state(StreamState new_state) {
  this->state_ = new_state;
  ESP_LOGD(TAG, "SET_STATE: State changed to %s", stream_state_to_string(new_state));
}

}  // namespace elevenlabs_stream
}  // namespace esphome
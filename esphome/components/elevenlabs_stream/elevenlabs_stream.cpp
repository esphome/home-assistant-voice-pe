#include "elevenlabs_stream.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/components/json/json_util.h"

#ifdef USE_ESP32
#include <WiFiClient.h>
#include <mbedtls/base64.h>

namespace esphome {
namespace elevenlabs_stream {

using namespace esphome::json;

static const char *const TAG = "elevenlabs_stream";

// ElevenLabs WebSocket URL components
static const char *const ELEVENLABS_HOST = "api.elevenlabs.io";
static const int ELEVENLABS_PORT = 80;
static const char *const ELEVENLABS_PATH = "/v1/convai/conversation";

void ElevenLabsStream::setup() {
  ESP_LOGCONFIG(TAG, "Setting up ElevenLabs Stream...");
  
  if (!this->microphone_) {
    ESP_LOGE(TAG, "Microphone not configured");
    this->mark_failed();
    return;
  }
  
  if (!this->speaker_) {
    ESP_LOGE(TAG, "Speaker not configured");
    this->mark_failed();
    return;
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
    
    // Check for handshake response
    if (this->wifi_client_.available()) {
      String response = this->wifi_client_.readStringUntil('\n');
      if (response.indexOf("HTTP/1.1 101") >= 0) {
        ESP_LOGI(TAG, "WebSocket handshake successful");
        this->websocket_connected_ = true;
        this->set_state(StreamState::CONNECTED);
        
        // Send initial conversation setup
        this->send_conversation_init();
        
        // Trigger connected events
        for (auto *trigger : this->on_connected_triggers_) {
          trigger->trigger();
        }
      } else if (response.indexOf("HTTP/1.1") >= 0) {
        this->handle_error(std::string("WebSocket handshake failed: ") + response.c_str());
        return;
      }
    }
  }
  
  // Handle incoming WebSocket messages
  if (this->websocket_connected_ && this->wifi_client_.available()) {
    this->process_websocket_data();
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
  this->connect_to_elevenlabs();
  return true;
}

void ElevenLabsStream::stop_stream() {
  ESP_LOGI(TAG, "Stopping ElevenLabs stream...");
  this->disconnect_from_elevenlabs();
  this->set_state(StreamState::IDLE);
}

void ElevenLabsStream::connect_to_elevenlabs() {
  ESP_LOGI(TAG, "Connecting to ElevenLabs...");
  
  // Generate WebSocket key
  uint8_t key_bytes[16];
  for (int i = 0; i < 16; i++) {
    key_bytes[i] = random(256);
  }
  
  // Base64 encode using mbedtls
  size_t olen;
  unsigned char output[32];
  mbedtls_base64_encode(output, sizeof(output), &olen, key_bytes, 16);
  output[olen] = '\0'; // Null terminate
  this->websocket_key_ = std::string((char*)output);
  
  // Connect to ElevenLabs
  if (!this->wifi_client_.connect(ELEVENLABS_HOST, ELEVENLABS_PORT)) {
    this->handle_error("Failed to connect to ElevenLabs server");
    return;
  }
  
  // Send WebSocket handshake
  String handshake = "GET " + String(ELEVENLABS_PATH) + " HTTP/1.1\r\n";
  handshake += "Host: " + String(ELEVENLABS_HOST) + "\r\n";
  handshake += "Upgrade: websocket\r\n";
  handshake += "Connection: Upgrade\r\n";
  handshake += "Sec-WebSocket-Key: " + String(this->websocket_key_.c_str()) + "\r\n";
  handshake += "Sec-WebSocket-Version: 13\r\n";
  if (!this->api_key_.empty()) {
    handshake += "xi-api-key: " + String(this->api_key_.c_str()) + "\r\n";
  }
  handshake += "\r\n";
  
  this->wifi_client_.print(handshake);
  
  // Set connecting state and wait for handshake response
  this->connection_start_time_ = millis();
}

void ElevenLabsStream::disconnect_from_elevenlabs() {
  ESP_LOGD(TAG, "Disconnecting from ElevenLabs...");
  this->websocket_connected_ = false;
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
  if (!this->websocket_connected_) {
    ESP_LOGW(TAG, "Cannot send message - WebSocket not connected");
    return;
  }
  
  // Create WebSocket text frame
  size_t payload_length = message.length();
  size_t frame_size = 2 + payload_length; // Header + payload
  
  if (payload_length >= 126) {
    frame_size += 2; // Extended length
  }
  if (payload_length >= 65536) {
    frame_size += 6; // 64-bit length (not supported in this simple implementation)
    ESP_LOGE(TAG, "Message too long");
    return;
  }
  
  // Build frame
  uint8_t *frame = new uint8_t[frame_size + 4]; // +4 for mask
  int pos = 0;
  
  // First byte: FIN=1, opcode=1 (text)
  frame[pos++] = 0x81;
  
  // Second byte: MASK=1, payload length
  if (payload_length < 126) {
    frame[pos++] = 0x80 | payload_length;
  } else {
    frame[pos++] = 0x80 | 126;
    frame[pos++] = (payload_length >> 8) & 0xFF;
    frame[pos++] = payload_length & 0xFF;
  }
  
  // Masking key
  uint32_t mask = random(0xFFFFFFFF);
  frame[pos++] = (mask >> 24) & 0xFF;
  frame[pos++] = (mask >> 16) & 0xFF;
  frame[pos++] = (mask >> 8) & 0xFF;
  frame[pos++] = mask & 0xFF;
  
  // Masked payload
  for (size_t i = 0; i < payload_length; i++) {
    frame[pos++] = message[i] ^ ((uint8_t*)&mask)[i % 4];
  }
  
  // Send frame
  this->wifi_client_.write(frame, pos);
  delete[] frame;
  
  ESP_LOGD(TAG, "Sent WebSocket message: %s", message.c_str());
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

void ElevenLabsStream::send_audio_chunk(const std::vector<int16_t> &audio_data) {
  if (!this->websocket_connected_ || audio_data.empty()) {
    return;
  }
  
  // Create binary WebSocket frame for audio
  size_t payload_size = audio_data.size() * sizeof(int16_t);
  size_t frame_size = 2 + payload_size + 4; // Header + payload + mask
  
  uint8_t *frame = new uint8_t[frame_size];
  int pos = 0;
  
  // First byte: FIN=1, opcode=2 (binary)
  frame[pos++] = 0x82;
  
  // Second byte: MASK=1, payload length
  if (payload_size < 126) {
    frame[pos++] = 0x80 | payload_size;
  } else {
    frame[pos++] = 0x80 | 126;
    frame[pos++] = (payload_size >> 8) & 0xFF;
    frame[pos++] = payload_size & 0xFF;
  }
  
  // Masking key
  uint32_t mask = random(0xFFFFFFFF);
  frame[pos++] = (mask >> 24) & 0xFF;
  frame[pos++] = (mask >> 16) & 0xFF;
  frame[pos++] = (mask >> 8) & 0xFF;
  frame[pos++] = mask & 0xFF;
  
  // Masked audio payload
  const uint8_t *audio_bytes = (const uint8_t*)audio_data.data();
  for (size_t i = 0; i < payload_size; i++) {
    frame[pos++] = audio_bytes[i] ^ ((uint8_t*)&mask)[i % 4];
  }
  
  // Send frame
  this->wifi_client_.write(frame, pos);
  delete[] frame;
  
  ESP_LOGV(TAG, "Sent audio chunk: %d samples", audio_data.size());
}

void ElevenLabsStream::handle_audio_response(const uint8_t *data, size_t length) {
  ESP_LOGD(TAG, "Playing audio response: %d bytes", length);
  
  if (this->speaker_ && length > 0) {
    // Play audio through speaker
    this->speaker_->play(data, length);
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

void ElevenLabsStream::process_websocket_data() {
  // Read WebSocket frames from the client
  while (this->wifi_client_.available()) {
    uint8_t byte = this->wifi_client_.read();
    
    // Simple frame parsing - in a real implementation you'd need
    // proper WebSocket frame parsing with state machine
    static String message_buffer;
    static bool in_text_frame = false;
    
    if (byte == 0x81) { // Text frame start
      in_text_frame = true;
      message_buffer = "";
    } else if (byte == 0x82) { // Binary frame start
      in_text_frame = false;
    } else if (in_text_frame && byte >= 0x20) { // Printable characters
      message_buffer += (char)byte;
    } else if (byte == 0x00 || !this->wifi_client_.available()) { // Frame end
      if (in_text_frame && message_buffer.length() > 0) {
        this->handle_websocket_message(message_buffer.c_str());
        message_buffer = "";
      }
      in_text_frame = false;
    }
  }
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

}  // namespace elevenlabs_stream
}  // namespace esphome

#endif  // USE_ESP32

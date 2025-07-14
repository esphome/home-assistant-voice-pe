#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/components/network/ip_address.h"
#include "esphome/components/json/json_util.h"

#ifdef USE_ESP32
#include <WiFi.h>
#include <WiFiClient.h>
#include <mbedtls/base64.h>
#endif

namespace esphome {
namespace elevenlabs_stream {

enum class StreamState {
  IDLE,
  CONNECTING,
  CONNECTED,
  LISTENING,
  SPEAKING,
  ERROR
};

class ElevenLabsStream : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_agent_id(const std::string &agent_id) { this->agent_id_ = agent_id; }
  void set_api_key(const std::string &api_key) { this->api_key_ = api_key; }
  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }
  void set_speaker(speaker::Speaker *speaker) { this->speaker_ = speaker; }

  bool start_stream();
  void stop_stream();
  bool is_running() const { return this->state_ != StreamState::IDLE; }
  bool is_connected() const { return this->websocket_connected_; }
  StreamState get_state() const { return this->state_; }

  // Triggers
  void add_on_start_trigger(Trigger<> *trigger) { this->on_start_triggers_.push_back(trigger); }
  void add_on_end_trigger(Trigger<> *trigger) { this->on_end_triggers_.push_back(trigger); }
  void add_on_listening_trigger(Trigger<> *trigger) { this->on_listening_triggers_.push_back(trigger); }
  void add_on_speaking_trigger(Trigger<> *trigger) { this->on_speaking_triggers_.push_back(trigger); }
  void add_on_connected_trigger(Trigger<> *trigger) { this->on_connected_triggers_.push_back(trigger); }
  void add_on_disconnected_trigger(Trigger<> *trigger) { this->on_disconnected_triggers_.push_back(trigger); }
  void add_on_error_trigger(Trigger<std::string> *trigger) { this->on_error_triggers_.push_back(trigger); }

 protected:
  void connect_to_elevenlabs();
  void disconnect_from_elevenlabs();
  void send_websocket_message(const std::string &message);
  void handle_websocket_message(const char *message);
  void handle_websocket_binary(const uint8_t *data, size_t length);
  void send_audio_chunk(const std::vector<int16_t> &audio_data);
  void handle_audio_response(const uint8_t *data, size_t length);
  void set_state(StreamState new_state);
  void handle_error(const std::string &error_message);
  void websocket_task();
  void send_conversation_init();
  void process_websocket_data();
  void capture_and_send_audio();
  void send_ping();

  std::string agent_id_;
  std::string api_key_;
  microphone::Microphone *microphone_{nullptr};
  speaker::Speaker *speaker_{nullptr};

  StreamState state_{StreamState::IDLE};

#ifdef USE_ESP32
  WiFiClient wifi_client_;
  bool websocket_connected_{false};
  std::string websocket_key_;
  std::string conversation_id_;
#endif

  // Triggers
  std::vector<Trigger<> *> on_start_triggers_;
  std::vector<Trigger<> *> on_end_triggers_;
  std::vector<Trigger<> *> on_listening_triggers_;
  std::vector<Trigger<> *> on_speaking_triggers_;
  std::vector<Trigger<> *> on_connected_triggers_;
  std::vector<Trigger<> *> on_disconnected_triggers_;
  std::vector<Trigger<std::string> *> on_error_triggers_;

  // Audio buffering
  std::vector<int16_t> audio_buffer_;
  std::vector<uint8_t> response_audio_buffer_;
  
  // Timing
  uint32_t last_audio_time_{0};
  uint32_t connection_timeout_{10000};  // 10 seconds
  uint32_t connection_start_time_{0};
  uint32_t last_heartbeat_{0};
};

// Actions
template<typename... Ts> class ElevenLabsStreamStartAction : public Action<Ts...>, public Parented<ElevenLabsStream> {
 public:
  void play(Ts... x) override { this->parent_->start_stream(); }
};

template<typename... Ts> class ElevenLabsStreamStopAction : public Action<Ts...>, public Parented<ElevenLabsStream> {
 public:
  void play(Ts... x) override { this->parent_->stop_stream(); }
};

// Triggers
class ElevenLabsStreamStartTrigger : public Trigger<> {};
class ElevenLabsStreamEndTrigger : public Trigger<> {};
class ElevenLabsStreamListeningTrigger : public Trigger<> {};
class ElevenLabsStreamSpeakingTrigger : public Trigger<> {};
class ElevenLabsStreamConnectedTrigger : public Trigger<> {};
class ElevenLabsStreamDisconnectedTrigger : public Trigger<> {};
class ElevenLabsStreamErrorTrigger : public Trigger<std::string> {};

// Condition
template<typename... Ts> class ElevenLabsStreamIsRunningCondition : public Condition<Ts...> {
 public:
  ElevenLabsStreamIsRunningCondition(ElevenLabsStream *parent) : parent_(parent) {}
  
  bool check(Ts... x) override { return this->parent_->is_running(); }
  
  void set_parent(ElevenLabsStream *parent) { this->parent_ = parent; }
  
 protected:
  ElevenLabsStream *parent_;
};

}  // namespace elevenlabs_stream
}  // namespace esphome

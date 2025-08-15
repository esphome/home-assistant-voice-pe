#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/components/network/ip_address.h"
#include "esphome/components/audio/audio.h"
#include "elevenlabs_client.h"

#include <esp_websocket_client.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <mbedtls/base64.h>

namespace esphome {

// Forward declarations for optional components
namespace microphone { class Microphone; }
namespace speaker { class Speaker; }

namespace elevenlabs_stream {

class ElevenLabsClient;
class ElevenLabsStream;
void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

enum class StreamState {
  OFF,    // Connection closed, not listening
  ON      // Connected and streaming audio both ways
};

class ElevenLabsStream : public Component {
  // Sets the speaker's audio stream info based on the agent output format, if available.
  void set_speaker_stream_info_to_elevenlabs_format();
public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_CONNECTION; }

  void set_agent_id(const std::string &agent_id) { this->agent_id_ = agent_id; }
  void set_api_key(const std::string &api_key) { this->api_key_ = api_key; }
  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }
  void set_elevenlabs_speaker(speaker::Speaker *speaker) { this->elevenlabs_speaker_ = speaker; }
  void set_activation_speaker(speaker::Speaker *speaker) { this->activation_speaker_ = speaker; }

  bool start_stream();
  void stop_stream();
  bool is_running() const { return this->state_ == StreamState::ON; }
  StreamState get_state() const { return this->state_; }
  void handle_microphone_data(const std::vector<uint8_t> &data);
  void handle_websocket_disconnected();

  // Speaker activity tracking
  bool is_speaker_active() const;

  // Triggers - simplified to just on/off and error
  void add_on_start_trigger(Trigger<> *trigger) { this->on_start_triggers_.push_back(trigger); }
  void add_on_end_trigger(Trigger<> *trigger) { this->on_end_triggers_.push_back(trigger); }
  void add_on_error_trigger(Trigger<std::string> *trigger) { this->on_error_triggers_.push_back(trigger); }
  void add_on_listening_trigger(Trigger<> *trigger) { this->on_listening_triggers_.push_back(trigger); }
  void add_on_processing_trigger(Trigger<> *trigger) { this->on_processing_triggers_.push_back(trigger); }
  void add_on_replying_trigger(Trigger<> *trigger) { this->on_replying_triggers_.push_back(trigger); }

  bool is_connected() const {
    return this->client_ && this->client_->is_connected();
  }

 protected:
  friend void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

  // Internal methods
  void renew_signed_url_if_needed();
  bool send_websocket_message(const std::string &message);
  void handle_websocket_message(const uint8_t *buffer, size_t length);
  void parse_json_message_from_buffer(const uint8_t *buffer, size_t length);
  void handle_error(const std::string &error_message);
  void send_conversation_init();
  void capture_and_send_audio();
  void send_ping();
  void send_audio_chunk(const std::vector<int16_t> &audio_data);
  void set_state(StreamState new_state);
  bool decode_and_play_base64_audio(const char* base64_data);

  std::string agent_id_;
  std::string api_key_;
  microphone::Microphone *microphone_{nullptr};
  speaker::Speaker *elevenlabs_speaker_{nullptr};
  speaker::Speaker *activation_speaker_{nullptr};
  ElevenLabsClient* client_ = nullptr;
  StreamState state_{StreamState::OFF};

  // Protocol state members for ElevenLabs API
  std::string conversation_id_;
  std::string agent_output_audio_format_;
  std::string user_input_audio_format_;
  std::string signed_url_; // Stores the current signed URL for ElevenLabs WebSocket

  // Triggers - simplified
  std::vector<Trigger<> *> on_start_triggers_;
  std::vector<Trigger<> *> on_end_triggers_;
  std::vector<Trigger<std::string> *> on_error_triggers_;
  std::vector<Trigger<> *> on_listening_triggers_;
  std::vector<Trigger<> *> on_processing_triggers_;
  std::vector<Trigger<> *> on_replying_triggers_;

  // Audio buffering
  std::vector<int16_t> audio_buffer_;
  std::vector<uint8_t> response_audio_buffer_;
  
  // WebSocket message fragmentation handling
  WebsocketMessageAssembler reassembler_;
  
  // Timing and configuration constants
  uint32_t last_audio_time_{0};
  uint32_t last_audio_response_time_{0};  // Track when we last received audio from agent
  uint32_t connection_timeout_{10000};  // Reduced to 10 seconds
  uint32_t connection_start_time_{0};
  uint32_t last_heartbeat_{0};
  uint32_t last_signed_url_renewal_{0};  // Track when we last renewed the signed URL
  uint32_t signed_url_renewal_interval_{600000};  // 10 minutes in milliseconds

  // Accumulated playback duration for all segments
  uint32_t accumulated_duration_ms_{0};
  
  // Speaker activity tracking to prevent microphone echo/feedback
  bool speaker_is_active_{false};  // Track if agent is currently speaking
  uint32_t speaker_start_time_{0};  // When current audio playback started
  uint32_t speaker_end_time_{0};   // When current audio playback should end
  uint32_t speaker_silence_buffer_ms_{500};  // Wait time after speaker stops before enabling mic

  // Last VAD score for tracking voice activity detection
  float last_vad_score_ = 0.0f;

  // Initial audio stream info
  esphome::audio::AudioStreamInfo activation_speaker_audio_stream_info{};
  bool activation_speaker_audio_stream_infoset_ = false;
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

// Triggers - simplified
class ElevenLabsStreamStartTrigger : public Trigger<> {};
class ElevenLabsStreamEndTrigger : public Trigger<> {};
class ElevenLabsStreamErrorTrigger : public Trigger<std::string> {};
class ElevenLabsStreamListeningTrigger : public Trigger<> {};
class ElevenLabsStreamProcessingTrigger : public Trigger<> {};
class ElevenLabsStreamReplyingTrigger : public Trigger<> {};

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

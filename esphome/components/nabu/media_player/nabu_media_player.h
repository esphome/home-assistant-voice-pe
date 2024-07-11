#pragma once

#ifdef USE_ESP_IDF

#include "esphome/components/media_player/media_player.h"
// #include "esphome/components/speaker/speaker.h"
#include "esphome/components/i2s_audio/speaker/i2s_audio_speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

// #include "mp3_decoder.h"
#include "streamer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_http_client.h>

namespace esphome {
namespace nabu {

enum class PipelineState : uint8_t {
  STARTING,
  STARTED,
  PLAYING,
  PAUSED,
  STOPPING,
  STOPPED,
};

struct MediaCallCommand {
  optional<media_player::MediaPlayerCommand> command;
  optional<float> volume;
  optional<bool> announce;
  optional<bool> new_url;
};

class NabuMediaPlayer : public Component, public media_player::MediaPlayer {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::LATE; }
  void setup() override;
  void loop() override;

  // MediaPlayer implementations
  bool is_muted() const override { return this->is_muted_; }
  media_player::MediaPlayerTraits get_traits() override;

  void start() {}
  void stop() {}
  void set_speaker(i2s_audio::I2SAudioSpeaker *speaker) { this->speaker_ = speaker; }

  void set_ducking_ratio(float ducking_ratio) override;

 protected:
  // Receives commands from HA or from the voice assistant component
  // Sends commands to the media_control_commanda_queue_
  void control(const media_player::MediaPlayerCall &call) override;
  optional<std::string> media_url_{};         // only modified by control function
  optional<std::string> announcement_url_{};  // only modified by control function
  QueueHandle_t media_control_command_queue_;

  // Reads commands from media_control_command_queue_. Starts pipelines and mixer if necessary. Writes to the pipeline command queues
  void watch_media_commands_();
  std::unique_ptr<Pipeline> media_pipeline_;
  std::unique_ptr<Pipeline> announcement_pipeline_;
  std::unique_ptr<CombineStreamer> combine_streamer_;

  // Monitors the pipelines' and mixer's event queues. Only function that modifies pipeline_state_ variables
  void watch_();
  PipelineState media_pipeline_state_{PipelineState::STOPPED};
  PipelineState announcement_pipeline_state_{PipelineState::STOPPED};
  
  i2s_audio::I2SAudioSpeaker *speaker_{nullptr};

  // // Transfer buffer for storing audio samples read from the mixer and then written to the speaker
  // uint8_t *transfer_buffer_{nullptr};

  bool is_paused_{false};
  bool is_muted_{false};

  // speaker::StreamInfo stream_info_{
  //         .channels = speaker::CHANNELS_MONO, .bits_per_sample = speaker::SAMPLE_BITS_16, .sample_rate = 16000};
};

}  // namespace nabu
}  // namespace esphome

#endif

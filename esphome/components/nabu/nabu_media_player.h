#pragma once

#ifdef USE_ESP_IDF

#include "esphome/components/media_player/media_player.h"
#include "esphome/components/i2s_audio/i2s_audio.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

#include "streamer.h"
#include "pipeline.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_http_client.h>

#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace nabu {

static const uint8_t DAC_PAGE_SELECTION_REGISTER = 0x00;
static const uint8_t DAC_LEFT_MUTE_REGISTER = 0x12;
static const uint8_t DAC_RIGHT_MUTE_REGISTER = 0x13;
static const uint8_t DAC_LEFT_VOLUME_REGISTER = 0x41;
static const uint8_t DAC_RIGHT_VOLUME_REGISTER = 0x42;

static const uint8_t DAC_VOLUME_PAGE = 0x00;
static const uint8_t DAC_MUTE_PAGE = 0x01;

static const uint8_t DAC_MUTE_COMMAND = 0x40;
static const uint8_t DAC_UNMUTE_COMMAND = 0x00;

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
  optional<bool> new_file;
};

// struct MediaFile {
//   const uint8_t *data;
//   MediaFileType file_type;
// };

class NabuMediaPlayer : public Component,
                        public media_player::MediaPlayer,
                        public i2s_audio::I2SAudioOut,
                        public i2c::I2CDevice {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::LATE; }
  void setup() override;
  void loop() override;

  // MediaPlayer implementations
  media_player::MediaPlayerTraits get_traits() override;
  bool is_muted() const override { return this->is_muted_; }

  void set_ducking_ratio(float ducking_ratio) override;

  void set_dout_pin(uint8_t pin) { this->dout_pin_ = pin; }
  void set_bits_per_sample(i2s_bits_per_sample_t bits_per_sample) { this->bits_per_sample_ = bits_per_sample; }

 protected:
  // Receives commands from HA or from the voice assistant component
  // Sends commands to the media_control_commanda_queue_
  void control(const media_player::MediaPlayerCall &call) override;

  /// @return volume read from DAC between 0.0 and 1.0, if successful
  optional<float> get_dac_volume_(bool publish = true);

  /// @return true if I2C writes were successful
  bool set_volume_(float volume, bool publish = true);

  /// @return true if I2C writes were successful
  bool mute_();

  /// @return true if I2C writes were successful
  bool unmute_();

  optional<std::string> media_url_{};                        // only modified by control function
  optional<std::string> announcement_url_{};                 // only modified by control function
  optional<media_player::MediaFile *> media_file_{};         // only modified by control fucntion
  optional<media_player::MediaFile *> announcement_file_{};  // only modified by control fucntion
  QueueHandle_t media_control_command_queue_;

  // Reads commands from media_control_command_queue_. Starts pipelines and mixer if necessary. Writes to the pipeline
  // command queues
  void watch_media_commands_();
  std::unique_ptr<Pipeline> media_pipeline_;
  std::unique_ptr<Pipeline> announcement_pipeline_;
  std::unique_ptr<CombineStreamer> combine_streamer_;

  // Monitors the pipelines' and mixer's event queues. Only function that modifies pipeline_state_ variables
  void watch_();
  PipelineState media_pipeline_state_{PipelineState::STOPPED};
  PipelineState announcement_pipeline_state_{PipelineState::STOPPED};

  void watch_speaker_();
  static void speaker_task(void *params);
  TaskHandle_t speaker_task_handle_{nullptr};
  QueueHandle_t speaker_event_queue_;
  QueueHandle_t speaker_command_queue_;

  i2s_bits_per_sample_t bits_per_sample_;
  uint8_t dout_pin_{0};

  bool is_paused_{false};
  bool is_muted_{false};

  // We mute the DAC whenever there is no audio playback to avoid speaker hiss
  bool is_idle_muted_{false};
};

}  // namespace nabu
}  // namespace esphome

#endif

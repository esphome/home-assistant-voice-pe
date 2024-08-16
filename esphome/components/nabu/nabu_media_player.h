#pragma once

#ifdef USE_ESP_IDF

#include "audio_mixer.h"
#include "audio_pipeline.h"

#include "esphome/components/media_player/media_player.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/i2s_audio/i2s_audio.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_http_client.h>

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

struct MediaCallCommand {
  optional<media_player::MediaPlayerCommand> command;
  optional<float> volume;
  optional<bool> announce;
  optional<bool> new_url;
  optional<bool> new_file;
};

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

  /// @brief Sets the ducking level for the media stream in the mixer
  /// @param decibel_reduction (uint8_t) The dB reduction level. For example, 0 is no change, 10 is a reduction by 10 dB
  /// @param duration (float) The duration (in seconds) for transitioning to the new ducking level
  void set_ducking_reduction(uint8_t decibel_reduction, float duration);

  void set_dout_pin(uint8_t pin) { this->dout_pin_ = pin; }
  void set_bits_per_sample(i2s_bits_per_sample_t bits_per_sample) { this->bits_per_sample_ = bits_per_sample; }
  void set_sample_rate(uint32_t sample_rate) { this->sample_rate_ = sample_rate; }

  void set_volume_increment(float volume_increment) { this->volume_increment_ = volume_increment; }
  
  void reconfigure_dac_new_settings();
  void reconfigure_dac_old_settings();

 protected:
  // Receives commands from HA or from the voice assistant component
  // Sends commands to the media_control_commanda_queue_
  void control(const media_player::MediaPlayerCall &call) override;

  /// @return Volume read from DAC between 0.0 and 1.0, if successful. Updates volume_ if publish is true.
  optional<float> get_dac_volume_(bool publish = true);

  /// @return Mute status read from DAC, if successful. Updates is_muted_ if publish is true.
  optional<bool> get_dac_mute_(bool publish = true);

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

  // Reads commands from media_control_command_queue_. Starts pipelines and mixer if necessary.
  void watch_media_commands_();

  std::unique_ptr<AudioPipeline> media_pipeline_;
  std::unique_ptr<AudioPipeline> announcement_pipeline_;
  std::unique_ptr<AudioMixer> audio_mixer_;

  // Monitors the mixer task
  void watch_mixer_();

  // Starts the ``type`` pipeline with a ``url`` or file. Starts the mixer, pipeline, and speaker tasks if necessary.
  // Unpauses if starting media in paused state
  esp_err_t start_pipeline_(AudioPipelineType type, bool url);

  AudioPipelineState media_pipeline_state_{AudioPipelineState::STOPPED};
  AudioPipelineState announcement_pipeline_state_{AudioPipelineState::STOPPED};

  void watch_speaker_();
  static void speaker_task(void *params);
  TaskHandle_t speaker_task_handle_{nullptr};
  QueueHandle_t speaker_event_queue_;
  QueueHandle_t speaker_command_queue_;

  i2s_bits_per_sample_t bits_per_sample_;
  uint32_t sample_rate_;
  uint8_t dout_pin_{0};

  bool is_paused_{false};
  bool is_muted_{false};

  // We mute the DAC whenever there is no audio playback to avoid speaker hiss
  bool is_idle_muted_{false};

  // The amount to change the volume on volume up/down commands
  float volume_increment_;
};

template<typename... Ts> class DuckingSetAction : public Action<Ts...>, public Parented<NabuMediaPlayer> {
  TEMPLATABLE_VALUE(uint8_t, decibel_reduction)
  TEMPLATABLE_VALUE(float, duration)
  void play(Ts... x) override {
    this->parent_->set_ducking_reduction(this->decibel_reduction_.value(x...), this->duration_.value(x...));
  }
};

}  // namespace nabu
}  // namespace esphome

#endif

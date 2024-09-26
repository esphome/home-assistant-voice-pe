#pragma once

#ifdef USE_ESP32

#include "../i2s_audio.h"

#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace i2s_audio {

enum class TaskEventType : uint8_t {
  STARTING = 0,
  STARTED,
  STOPPING,
  STOPPED,
  WARNING = 255,
};

struct TaskEvent {
  TaskEventType type;
  esp_err_t err;
};

struct StreamInfo {
  bool operator==(const StreamInfo &rhs) const {
    return (channels == rhs.channels) && (bits_per_sample == rhs.bits_per_sample) && (sample_rate == rhs.sample_rate);
  }
  bool operator!=(const StreamInfo &rhs) const { return !operator==(rhs); }
  uint8_t channels = 1;
  uint8_t bits_per_sample = 16;
  uint32_t sample_rate = 16000;
};

class I2SAudioSpeaker : public I2SAudioOut, public speaker::Speaker, public Component {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::LATE; }

  void setup() override;
  void loop() override;

  void set_timeout(uint32_t ms) { this->timeout_ = ms; }
  void set_dout_pin(uint8_t pin) { this->dout_pin_ = pin; }
#if SOC_I2S_SUPPORTS_DAC
  void set_internal_dac_mode(i2s_dac_mode_t mode) { this->internal_dac_mode_ = mode; }
#endif
  void set_i2s_comm_fmt(i2s_comm_format_t mode) { this->i2s_comm_fmt_ = mode; }

  void start() override;
  void stop() override;
  void finish() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return play(data, length, 0); }

  bool has_buffered_data() const override;

  void set_num_channels(uint8_t num_channels) { this->num_channels_ = num_channels; }
  void set_sample_rate(uint8_t num_channels) { this->sample_rate_ = sample_rate; }

 protected:
  esp_err_t start_i2s_driver_();
  static void speaker_task(void *params);

  TaskHandle_t speaker_task_handle_{nullptr};
  QueueHandle_t speaker_event_queue_;

  std::unique_ptr<RingBuffer> audio_ring_buffer_;

  void stop_(bool wait_on_empty);
  void watch_();

  static void speaker_task_(void *params);

  uint32_t timeout_{0};
  uint8_t dout_pin_{0};
  bool task_created_{false};

  uint8_t num_channels_{1};
  uint32_t sample_rate_{16000};

#if SOC_I2S_SUPPORTS_DAC
  i2s_dac_mode_t internal_dac_mode_{I2S_DAC_CHANNEL_DISABLE};
#endif
  i2s_comm_format_t i2s_comm_fmt_;
};

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32

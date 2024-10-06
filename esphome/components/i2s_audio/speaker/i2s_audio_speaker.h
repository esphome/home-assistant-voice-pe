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

  esp_err_t set_i2s_stream_info_(StreamInfo &stream_info);

  void start() override;
  void stop() override;
  void finish() override;

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return play(data, length, 0); }

  bool has_buffered_data() const override;

  void set_volume(float volume) override;
  float get_volume() override { return this->volume_; }

 protected:
  esp_err_t start_i2s_driver_();
  static void speaker_task(void *params);

  TaskHandle_t speaker_task_handle_{nullptr};
  QueueHandle_t speaker_event_queue_;

  std::unique_ptr<RingBuffer> audio_ring_buffer_;

  void stop_(bool wait_on_empty);

  static void speaker_task_(void *params);

  /// @brief Multiplies the input array of Q15 numbers by a Q15 constant factor
  ///
  /// Based on `dsps_mulc_s16_ansi` from the esp-dsp library:
  /// https://github.com/espressif/esp-dsp/blob/master/modules/math/mulc/fixed/dsps_mulc_s16_ansi.c
  /// (accessed on 2024-09-30).
  /// @param input Array of Q15 numbers
  /// @param output Array of Q15 numbers
  /// @param len Length of array
  /// @param c Q15 constant factor
  static void q15_multiplication(const int16_t *input, int16_t *output, size_t len, int16_t c);

  uint32_t timeout_{0};
  uint8_t dout_pin_{0};
  bool task_created_{false};

  float volume_{1.0f};
  int16_t q15_volume_factor_{INT16_MAX};

#if SOC_I2S_SUPPORTS_DAC
  i2s_dac_mode_t internal_dac_mode_{I2S_DAC_CHANNEL_DISABLE};
#endif
  i2s_comm_format_t i2s_comm_fmt_;
};

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32

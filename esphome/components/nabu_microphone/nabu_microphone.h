#pragma once

#ifdef USE_ESP32

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "esphome/components/i2s_audio/i2s_audio.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace nabu_microphone {

enum class TaskEventType : uint8_t {
  STARTING = 0,
  STARTED,
  RUNNING,
  IDLE,
  STOPPING,
  STOPPED,
  MUTED,
  WARNING = 255,
};

struct TaskEvent {
  TaskEventType type;
  esp_err_t err;
};

class NabuMicrophoneChannel;

class NabuMicrophone : public i2s_audio::I2SAudioIn, public Component {
 public:
  void setup() override;
  void start();
  void stop();

  void loop() override;

  void mute();
  void unmute();

  void set_channel_0(NabuMicrophoneChannel *microphone) { this->channel_0_ = microphone; }
  void set_channel_1(NabuMicrophoneChannel *microphone) { this->channel_1_ = microphone; }

  NabuMicrophoneChannel *get_channel_0() { return this->channel_0_; }
  NabuMicrophoneChannel *get_channel_1() { return this->channel_1_; }

#if SOC_I2S_SUPPORTS_ADC
  void set_adc_channel(adc1_channel_t channel) {
    this->adc_channel_ = channel;
    this->adc_ = true;
  }
#endif

  void set_i2s_mode(i2s_mode_t mode) { this->i2s_mode_ = mode; }
  void set_sample_rate(uint32_t sample_rate) { this->sample_rate_ = sample_rate; }
  void set_bits_per_sample(i2s_bits_per_sample_t bits_per_sample) { this->bits_per_sample_ = bits_per_sample; }
  void set_use_apll(uint32_t use_apll) { this->use_apll_ = use_apll; }

  void set_din_pin(int8_t pin) { this->din_pin_ = pin; }
  void set_pdm(bool pdm) { this->pdm_ = pdm; }

  bool is_running() { return this->state_ == microphone::STATE_RUNNING; }
  uint32_t get_sample_rate() { return this->sample_rate_; }

 protected:
  esp_err_t start_i2s_driver_();

  microphone::State state_{microphone::STATE_STOPPED};

  static void read_task_(void *params);

  TaskHandle_t read_task_handle_{nullptr};
  QueueHandle_t event_queue_;

  NabuMicrophoneChannel *channel_0_{nullptr};
  NabuMicrophoneChannel *channel_1_{nullptr};

  bool use_apll_;
  bool pdm_{false};
  int8_t din_pin_{I2S_PIN_NO_CHANGE};

#if SOC_I2S_SUPPORTS_ADC
  bool adc_{false};
  adc1_channel_t adc_channel_{ADC1_CHANNEL_MAX};
#endif

  i2s_bits_per_sample_t bits_per_sample_;
  i2s_channel_fmt_t channel_;
  i2s_mode_t i2s_mode_{};
  uint32_t sample_rate_;
};

class NabuMicrophoneChannel : public microphone::Microphone, public Component {
 public:
  void setup() override;

  void start() override {
    this->parent_->start();
    this->is_muted_ = false;
    this->requested_stop_ = false;
  }

  void set_parent(NabuMicrophone *nabu_microphone) { this->parent_ = nabu_microphone; }

  void stop() override {
    this->requested_stop_ = true;
    this->is_muted_ = true;  // Mute until it is actually stopped
  };

  void loop() override;

  void set_mute_state(bool mute_state) override { this->is_muted_ = mute_state; }
  bool get_mute_state() { return this->is_muted_; }

  // void set_requested_stop() { this->requested_stop_ = true; }
  bool get_requested_stop() { return this->requested_stop_; }

  size_t read(int16_t *buf, size_t len, TickType_t ticks_to_wait = 0) override {
    return this->ring_buffer_->read((void *) buf, len, ticks_to_wait);
  };
  size_t read(int16_t *buf, size_t len) override { return this->ring_buffer_->read((void *) buf, len); };
  void reset() override { this->ring_buffer_->reset(); }

  RingBuffer *get_ring_buffer() { return this->ring_buffer_.get(); }

  void set_amplify_shift(uint8_t amplify_shift) { this->amplify_shift_ = amplify_shift; }
  uint8_t get_amplify_shift() { return this->amplify_shift_; }

 protected:
  NabuMicrophone *parent_;
  std::unique_ptr<RingBuffer> ring_buffer_;

  uint8_t amplify_shift_;
  bool is_muted_;
  bool requested_stop_;
};

}  // namespace nabu_microphone
}  // namespace esphome

#endif  // USE_ESP32

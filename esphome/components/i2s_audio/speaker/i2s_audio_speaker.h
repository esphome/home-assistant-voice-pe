#pragma once

#ifdef USE_ESP32

#include "../i2s_audio.h"

#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_http_client.h>

#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace i2s_audio {

enum FeedType : uint8_t {
  FILE,
  URL,
};

struct FeedCommandEvent {
  const uint8_t *data;
  size_t length;
  bool stop = false;
  FeedType feed_type;
};

enum StreamChannels :uint8_t {
  STREAM_CHANNELS_MONO = 1,
  STREAM_CHANNELS_STEREO = 2,
};
enum StreamBitsPerSample :uint8_t {
  STREAM_BITS_PER_SAMPLE_16 = I2S_BITS_PER_SAMPLE_16BIT,
  STREAM_BITS_PER_SAMPLE_32 = I2S_BITS_PER_SAMPLE_32BIT,
};
struct StreamInfo {
  StreamChannels channels = STREAM_CHANNELS_MONO;
  StreamBitsPerSample bits_per_sample = STREAM_BITS_PER_SAMPLE_16;
  uint32_t sample_rate = 16000;
};

class I2SAudioSpeaker : public Component, public speaker::Speaker, public I2SAudioOut {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::LATE; }

  void setup() override;
  void loop() override;

  void set_dout_pin(uint8_t pin) { this->dout_pin_ = pin; }
#if SOC_I2S_SUPPORTS_DAC
  void set_internal_dac_mode(i2s_dac_mode_t mode) { this->internal_dac_mode_ = mode; }
#endif
  void set_external_dac_channels(uint8_t channels) { this->external_dac_channels_ = channels; }
  void set_bits_per_sample(i2s_bits_per_sample_t bits_per_sample) { this->bits_per_sample_ = bits_per_sample; }

  bool is_playing() { return this->is_playing_; }

  void start() override;
  void stop() override;

  size_t play(const uint8_t *data, size_t length) override;
  size_t play_file(const uint8_t *data, size_t length);
  size_t play_url(const std::string &uri);
  
  // Directly writes to the input ring buffer
  size_t write(const uint8_t *data, size_t length);
  size_t free_bytes() { return this->input_ring_buffer_->free();}

  bool has_buffered_data() const override;

 protected:
  void start_();
  void watch_();
  void stop_();

  bool read_wav_header_();
  bool initiate_client_(const std::string &new_uri);
  
  static void player_task(void *params);
  static void feed_task(void *params);

  bool is_playing_{false};

  esp_http_client_handle_t client_ = nullptr;

  TaskHandle_t player_task_handle_{nullptr};
  TaskHandle_t feed_task_handle_{nullptr};

  QueueHandle_t play_event_queue_;
  QueueHandle_t play_command_queue_;

  QueueHandle_t feed_event_queue_;
  QueueHandle_t feed_command_queue_;

  std::unique_ptr<RingBuffer> input_ring_buffer_;

  i2s_bits_per_sample_t bits_per_sample_;

  uint8_t dout_pin_{0};

#if SOC_I2S_SUPPORTS_DAC
  i2s_dac_mode_t internal_dac_mode_{I2S_DAC_CHANNEL_DISABLE};
#endif
  uint8_t external_dac_channels_;
};

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32

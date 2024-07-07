#pragma once

#ifdef USE_ESP_IDF

#include "esphome/components/media_player/media_player.h"
// #include "esphome/components/speaker/speaker.h"
#include "esphome/components/i2s_audio/speaker/i2s_audio_speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

// #include "mp3_decoder.h"
#include "http_streamer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <esp_http_client.h>

namespace esphome {
namespace nabu {

class NabuMediaPlayer : public Component, public media_player::MediaPlayer {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::LATE; }
  void setup() override;
  void loop() override;

  // MediaPlayer implementations
  bool is_muted() const override { return this->muted_; }
  media_player::MediaPlayerTraits get_traits() override;

  void start() {}
  void stop() {}
  void set_speaker(i2s_audio::I2SAudioSpeaker *speaker) { this->speaker_ = speaker; }

 protected:
  // MediaPlayer implementation
  void control(const media_player::MediaPlayerCall &call) override;

  void watch_();

  bool muted_{false};
  bool play_intent_{false};

  std::unique_ptr<HTTPStreamer> media_streamer_;
  std::unique_ptr<HTTPStreamer> announcement_streamer_;

  i2s_audio::I2SAudioSpeaker *speaker_{nullptr};
  bool is_playing_{false};
  bool is_announcing_{false};
  uint8_t *transfer_buffer_{nullptr};

  int header_control_counter_{0};

  bool read_wav_header_(esp_http_client_handle_t *client);
  // speaker::StreamInfo stream_info_{
  //         .channels = speaker::CHANNELS_MONO, .bits_per_sample = speaker::SAMPLE_BITS_16, .sample_rate = 16000};
};

}  // namespace nabu
}  // namespace esphome

#endif

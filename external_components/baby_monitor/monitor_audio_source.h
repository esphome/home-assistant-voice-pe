#pragma once
#include "esphome/components/audio_http/audio_http_media_source.h"
namespace esphome::baby_monitor {
// Route a dedicated scheme through an independently registered HTTP decoder.
// AudioHTTPMediaSource is final: use composition, leaving normal TTS untouched.
class MonitorAudioSource : public Component,
                           public media_source::MediaSource,
                           public media_source::MediaSourceListener {
 public:
  void set_transport(audio_http::AudioHTTPMediaSource *transport) { transport_ = transport; }
  void setup() override { transport_->set_listener(this); this->disable_loop(); }
  bool can_handle(const std::string &uri) const override {
    return uri.rfind("monitor+http://", 0) == 0 || uri.rfind("monitor+https://", 0) == 0;
  }
  bool play_uri(const std::string &uri) override {
    return can_handle(uri) && transport_->play_uri(uri.substr(8));
  }
  void handle_command(media_source::MediaSourceCommand command) override { transport_->handle_command(command); }
  size_t write_audio(const uint8_t *data, size_t length, uint32_t timeout_ms,
                     const audio::AudioStreamInfo &info) override {
    return this->write_output(data, length, timeout_ms, info);
  }
  void report_state(media_source::MediaSourceState state) override { this->set_state_(state); }
 protected:
  audio_http::AudioHTTPMediaSource *transport_{nullptr};
};
}

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "esphome/core/audio.h"

namespace esphome {
namespace speaker {

enum State : uint8_t {
  STATE_STOPPED = 0,
  STATE_STARTING,
  STATE_RUNNING,
  STATE_STOPPING,
};

class Speaker {
 public:
  virtual size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
    return this->play(data, length);
  };
  virtual size_t play(const uint8_t *data, size_t length) = 0;
  size_t play(const std::vector<uint8_t> &data) { return this->play(data.data(), data.size()); }

  virtual void start() = 0;
  virtual void stop() = 0;
  // In compare between *STOP()* and *FINISH()*; *FINISH()* will stop after emptying the play buffer,
  // while *STOP()* will break directly.
  // When finish() is not implemented on the plateform component it should just do a normal stop.
  virtual void finish() { this->stop(); }

  virtual bool has_buffered_data() const = 0;

  bool is_running() const { return this->state_ == STATE_RUNNING; }
  bool is_stopped() const { return this->state_ == STATE_STOPPED; }

  virtual void set_volume(float volume) {};
  virtual float get_volume() { return 1.0f; }

  void set_audio_stream_info(const AudioStreamInfo &audio_stream_info) { this->audio_stream_info_ = audio_stream_info; }

 protected:
  State state_{STATE_STOPPED};
  AudioStreamInfo audio_stream_info_;
};

}  // namespace speaker
}  // namespace esphome

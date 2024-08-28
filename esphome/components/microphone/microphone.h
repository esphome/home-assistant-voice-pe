#pragma once

#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace microphone {

// TODO: The mute state should belong to the microphone, not the parent nabu_microphone

enum State : uint8_t {
  STATE_STOPPED = 0,
  STATE_STARTING,
  STATE_RUNNING,
  STATE_MUTED,
  STATE_STOPPING,
};

class Microphone {
 public:
  virtual void start() = 0;
  virtual void stop() = 0;
  void add_data_callback(std::function<void(const std::vector<int16_t> &)> &&data_callback) {
    this->data_callbacks_.add(std::move(data_callback));
  }
  virtual size_t read(int16_t *buf, size_t len) = 0;

  // How many bytes are available in the ring buffer
  virtual size_t available() { return 0; }

  // Reset the ring buffer
  virtual void reset() {}

  virtual void set_mute_state(bool mute_state) {};

  bool is_running() const { return this->state_ == STATE_RUNNING; }
  bool is_stopped() const { return this->state_ == STATE_STOPPED; }
  bool is_muted() const { return this->state_ == STATE_MUTED; }

 protected:
  State state_{STATE_STOPPED};

  CallbackManager<void(const std::vector<int16_t> &)> data_callbacks_{};
};

}  // namespace microphone
}  // namespace esphome

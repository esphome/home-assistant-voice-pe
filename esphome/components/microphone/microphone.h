#pragma once

#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <functional>
#include <vector>
#include "esphome/core/helpers.h"

namespace esphome {
namespace microphone {

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

  /// @brief Reads from the microphone blocking ticks_to_wait FreeRTOS ticks. Intended for use in tasks.
  virtual size_t read(int16_t *buf, size_t len, TickType_t ticks_to_wait) { return this->read(buf, len); }

  /// @brief If the microphone implementation uses a ring buffer, this will reset it - discarding all the stored data
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
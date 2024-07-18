#pragma once

#ifdef USE_ESP_IDF

#include "streamer.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace esphome {
namespace nabu {

class DecodeStreamer : public OutputStreamer {
 public:
  DecodeStreamer();
  void start(const std::string &task_name, UBaseType_t priority = 1) override;
  void reset_ring_buffers() override;

  size_t input_free() { return this->input_ring_buffer_->free(); }

  bool empty() { return (this->input_ring_buffer_->available() + this->output_ring_buffer_->available()) == 0; }

  size_t write(uint8_t *buffer, size_t length);

 protected:
  static void decode_task_(void *params);
  std::unique_ptr<RingBuffer> input_ring_buffer_;
};

}  // namespace nabu
}  // namespace esphome

#endif
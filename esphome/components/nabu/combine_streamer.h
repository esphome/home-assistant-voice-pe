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

class CombineStreamer : public OutputStreamer {
 public:
  CombineStreamer();

  void start(const std::string &task_name, UBaseType_t priority = 1) override;
  // void stop() override;
  void reset_ring_buffers() override;

  size_t media_free() { return this->media_ring_buffer_->free(); }
  size_t announcement_free() { return this->announcement_ring_buffer_->free(); }

  size_t write_media(uint8_t *buffer, size_t length);
  size_t write_announcement(uint8_t *buffer, size_t length);

  BaseType_t read_media_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->media_event_queue_, event, ticks_to_wait);
  }
  BaseType_t read_announcement_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->announcement_event_queue_, event, ticks_to_wait);
  }

 protected:
  static void combine_task_(void *params);

  std::unique_ptr<RingBuffer> media_ring_buffer_;
  std::unique_ptr<RingBuffer> announcement_ring_buffer_;

  QueueHandle_t media_event_queue_;
  QueueHandle_t announcement_event_queue_;
};
}  // namespace nabu
}  // namespace esphome

#endif
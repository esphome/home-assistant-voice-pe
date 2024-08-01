#pragma once

#ifdef USE_ESP_IDF

#include "esphome/components/media_player/media_player.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace esphome {
namespace nabu {

enum class EventType : uint8_t {
  STARTING = 0,
  STARTED,
  RUNNING,
  IDLE,
  STOPPING,
  STOPPED,
  WARNING = 255,
};

struct TaskEvent {
  EventType type;
  esp_err_t err;
};

enum class CommandEventType : uint8_t {
  START,
  STOP,
  DUCK,
  PAUSE_MEDIA,
  RESUME_MEDIA,
  CLEAR_MEDIA,
  CLEAR_ANNOUNCEMENT,
};

struct CommandEvent {
  CommandEventType command;
  float ducking_ratio = 0.0;
};

class AudioMixer {
 public:
  AudioMixer();

  /// @brief Returns the number of bytes available to read from the ring buffer
  size_t available() { return this->output_ring_buffer_->available(); }

  BaseType_t send_command(CommandEvent *command, TickType_t ticks_to_wait = portMAX_DELAY) {
    return xQueueSend(this->command_queue_, command, ticks_to_wait);
  }

  BaseType_t read_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->event_queue_, event, ticks_to_wait);
  }

  void start(const std::string &task_name, UBaseType_t priority = 1);

  void stop() {
    vTaskDelete(this->task_handle_);
    this->task_handle_ = nullptr;

    xQueueReset(this->event_queue_);
    xQueueReset(this->command_queue_);
  }

  void reset_ring_buffers();

  size_t media_free() { return this->media_ring_buffer_->free(); }
  size_t announcement_free() { return this->announcement_ring_buffer_->free(); }

  /// @brief Reads from the output ring buffer
  /// @param buffer stores the read data
  /// @param length how many bytes requested to read from the ring buffer
  /// @return number of bytes actually read; will be less than length if not available in ring buffer
  size_t read(uint8_t *buffer, size_t length, TickType_t ticks_to_wait = 0) {
    size_t available_bytes = this->available();
    size_t bytes_to_read = std::min(length, available_bytes);
    if (bytes_to_read > 0) {
      return this->output_ring_buffer_->read((void *) buffer, bytes_to_read, ticks_to_wait);
    }
    return 0;
  }

  size_t write_media(uint8_t *buffer, size_t length);
  size_t write_announcement(uint8_t *buffer, size_t length);

  BaseType_t read_media_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->media_event_queue_, event, ticks_to_wait);
  }
  BaseType_t read_announcement_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->announcement_event_queue_, event, ticks_to_wait);
  }

  RingBuffer *get_media_ring_buffer() { return this->media_ring_buffer_.get(); }
  RingBuffer *get_announcement_ring_buffer() { return this->announcement_ring_buffer_.get(); }

 protected:
  TaskHandle_t task_handle_{nullptr};
  StaticTask_t task_stack_;
  StackType_t *stack_buffer_{nullptr};

  std::unique_ptr<RingBuffer> output_ring_buffer_;
  QueueHandle_t event_queue_;
  QueueHandle_t command_queue_;

  static void mix_task_(void *params);

  std::unique_ptr<RingBuffer> media_ring_buffer_;
  std::unique_ptr<RingBuffer> announcement_ring_buffer_;

  QueueHandle_t media_event_queue_;
  QueueHandle_t announcement_event_queue_;
};
}  // namespace nabu
}  // namespace esphome

#endif
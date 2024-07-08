#pragma once

#ifdef USE_ESP_IDF

#include "esphome/core/ring_buffer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include <esp_http_client.h>

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
enum CommandEventType : uint8_t {
  START,
  STOP,
  DUCK,
};
struct CommandEvent {
  CommandEventType command;
  float ducking_ratio = 0.0;
};

class HTTPStreamer {
 public:
  HTTPStreamer();

  void start(UBaseType_t priority = 1) {
    if (this->task_handle_ == nullptr) {
      xTaskCreate(HTTPStreamer::read_task_, "read_task", 8096, (void *) this, priority, &this->task_handle_);
    }
  }

  void stop() {
    vTaskDelete(this->task_handle_);
    this->task_handle_ = nullptr;

    this->output_ring_buffer_->reset();
    xQueueReset(this->event_queue_);
    xQueueReset(this->command_queue_);
  }

  /// @brief Reads from the output ring buffer
  /// @param buffer stores the read data
  /// @param length how many bytes requested to read from the ring buffer
  /// @return number of bytes actually read; will be less than length if not available in ring buffer
  size_t read(uint8_t *buffer, size_t length) {
    size_t available_bytes = this->available();
    size_t bytes_to_read = std::min(length, available_bytes);
    if (bytes_to_read > 0) {
      return this->output_ring_buffer_->read((void *) buffer, bytes_to_read);
    }
    return 0;
  }

  /// @brief Returns the number of bytes available to read from the ring buffer
  size_t available() { return this->output_ring_buffer_->available(); }

  BaseType_t send_command(CommandEvent *command, TickType_t ticks_to_wait = portMAX_DELAY) {
    return xQueueSend(this->command_queue_, command, ticks_to_wait);
  }

  BaseType_t read_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->event_queue_, event, ticks_to_wait);
  }

  void set_current_uri(const std::string &current_uri) { this->current_uri_ = current_uri; }
  std::string get_current_uri() { return this->current_uri_; }

 protected:
  static void read_task_(void *params);

  void set_stream_uri_(esp_http_client_handle_t *client, const std::string &new_uri);
  void cleanup_(esp_http_client_handle_t *client);

  esp_http_client_handle_t client_ = nullptr;

  std::string current_uri_{};

  TaskHandle_t task_handle_{nullptr};
  std::unique_ptr<RingBuffer> output_ring_buffer_;
  QueueHandle_t event_queue_;
  QueueHandle_t command_queue_;
};

class CombineStreamer {
 public:
  CombineStreamer();

  void start(UBaseType_t priority = 1) {
    if (this->task_handle_ == nullptr) {
      xTaskCreate(CombineStreamer::combine_task_, "combine_task", 8096, (void *) this, priority, &this->task_handle_);
    }
  }

  void stop() {
    vTaskDelete(this->task_handle_);
    this->task_handle_ = nullptr;

    this->output_ring_buffer_->reset();
    this->media_ring_buffer_->reset();
    this->announcement_ring_buffer_->reset();
    xQueueReset(this->event_queue_);
    xQueueReset(this->command_queue_);
  }

  /// @brief Reads from the output ring buffer
  /// @param buffer stores the read data
  /// @param length how many bytes requested to read from the ring buffer
  /// @return number of bytes actually read; will be less than length if not available in ring buffer
  size_t read(uint8_t *buffer, size_t length) {
    size_t available_bytes = this->available();
    size_t bytes_to_read = std::min(length, available_bytes);
    if (bytes_to_read > 0) {
      return this->output_ring_buffer_->read((void *) buffer, bytes_to_read);
    }
    return 0;
  }

  size_t media_free() { return this->media_ring_buffer_->free(); }
  size_t announcement_free() { return this->announcement_ring_buffer_->free(); }

  size_t write_media(uint8_t *buffer, size_t length) {
    size_t free_bytes = this->media_free();
    size_t bytes_to_write = std::min(length, free_bytes);
    return this->media_ring_buffer_->write((void *) buffer, bytes_to_write);
  }

  size_t write_announcement(uint8_t *buffer, size_t length);
  // {
  //   size_t free_bytes = this->announcement_free();
  //   size_t bytes_to_write = std::min(length, free_bytes);
  //   size_t bytes_written = this->announcement_ring_buffer_->write((void *) buffer, bytes_to_write);
  //   ESP_LOGD("combiner", "wrote %d bytes to announcemnet ring buffer", bytes_written);
  //   // return this->announcement_ring_buffer_->write((void *) buffer, bytes_to_write);
  //   return bytes_written;
  // }

  /// @brief Returns the number of bytes available to read from the ring buffer
  size_t available() { return this->output_ring_buffer_->available(); }

  BaseType_t send_command(CommandEvent *command, TickType_t ticks_to_wait = portMAX_DELAY) {
    return xQueueSend(this->command_queue_, command, ticks_to_wait);
  }

  BaseType_t read_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->event_queue_, event, ticks_to_wait);
  }

 protected:
  static void combine_task_(void *params);

  TaskHandle_t task_handle_{nullptr};
  std::unique_ptr<RingBuffer> output_ring_buffer_;
  std::unique_ptr<RingBuffer> media_ring_buffer_;
  std::unique_ptr<RingBuffer> announcement_ring_buffer_;
  QueueHandle_t event_queue_;
  QueueHandle_t command_queue_;
};

}  // namespace nabu
}  // namespace esphome
#endif
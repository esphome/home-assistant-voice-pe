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

class OutputStreamer {
 public:
  /// @brief Returns the number of bytes available to read from the ring buffer
  size_t available() { return this->output_ring_buffer_->available(); }

  BaseType_t send_command(CommandEvent *command, TickType_t ticks_to_wait = portMAX_DELAY) {
    return xQueueSend(this->command_queue_, command, ticks_to_wait);
  }

  BaseType_t read_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->event_queue_, event, ticks_to_wait);
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

  virtual void start(UBaseType_t priority = 1) = 0;
  virtual void stop() = 0;

 protected:
  TaskHandle_t task_handle_{nullptr};
  std::unique_ptr<RingBuffer> output_ring_buffer_;
  QueueHandle_t event_queue_;
  QueueHandle_t command_queue_;
};

class HTTPStreamer : public OutputStreamer {
 public:
  HTTPStreamer();

  void start(UBaseType_t priority = 1) override;
  void stop() override;

  void set_current_uri(const std::string &current_uri) { this->current_uri_ = current_uri; }
  std::string get_current_uri() { return this->current_uri_; }

 protected:
  static void read_task_(void *params);

  void set_stream_uri_(esp_http_client_handle_t *client, const std::string &new_uri);
  void cleanup_(esp_http_client_handle_t *client);

  // esp_http_client_handle_t client_ = nullptr;

  std::string current_uri_{};
};

class CombineStreamer : public OutputStreamer {
 public:
  CombineStreamer();

  void start(UBaseType_t priority = 1) override;
  void stop() override;

  size_t media_free() { return this->media_ring_buffer_->free(); }
  size_t announcement_free() { return this->announcement_ring_buffer_->free(); }

  size_t write_media(uint8_t *buffer, size_t length);
  size_t write_announcement(uint8_t *buffer, size_t length);

 protected:
  static void combine_task_(void *params);

  std::unique_ptr<RingBuffer> media_ring_buffer_;
  std::unique_ptr<RingBuffer> announcement_ring_buffer_;
};

}  // namespace nabu
}  // namespace esphome
#endif
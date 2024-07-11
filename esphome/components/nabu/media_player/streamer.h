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
enum class CommandEventType : uint8_t {
  START,
  STOP,
  STOP_GRACEFULLY,
  DUCK,
  PAUSE_MEDIA,
  RESUME_MEDIA,
};
struct CommandEvent {
  CommandEventType command;
  float ducking_ratio = 0.0;
};

enum class PipelineType : uint8_t {
  MEDIA,
  ANNOUNCEMENT,
};

static const size_t QUEUE_COUNT = 10;

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
  size_t read(uint8_t *buffer, size_t length, TickType_t ticks_to_wait = 0) {
    size_t available_bytes = this->available();
    size_t bytes_to_read = std::min(length, available_bytes);
    if (bytes_to_read > 0) {
      return this->output_ring_buffer_->read((void *) buffer, bytes_to_read, ticks_to_wait);
    }
    return 0;
  }

  virtual void reset_ring_buffers() { this->output_ring_buffer_->reset();}

  virtual void start(UBaseType_t priority = 1) = 0;
  virtual void stop();

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
  void start(const std::string &uri, UBaseType_t priority = 1);

 protected:
  static void read_task_(void *params);

  void establish_connection_(esp_http_client_handle_t *client);
  void cleanup_connection_(esp_http_client_handle_t *client);

  std::string current_uri_{};
};

class DecodeStreamer : public OutputStreamer {
 public:
  DecodeStreamer();
  void start(UBaseType_t priority = 1) override;
  void reset_ring_buffers() override;

  size_t input_free() { return this->input_ring_buffer_->free(); }
  
  bool empty() { return (this->input_ring_buffer_->available() + this->output_ring_buffer_->available()) == 0;}

  size_t write(uint8_t *buffer, size_t length);

 protected:
  static void decode_task_(void *params);
  std::unique_ptr<RingBuffer> input_ring_buffer_;
};

class CombineStreamer : public OutputStreamer {
 public:
  CombineStreamer();

  void start(UBaseType_t priority = 1) override;
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

class Pipeline {
 public:
  Pipeline(CombineStreamer *mixer, PipelineType pipeline_type) {
    this->reader_ = make_unique<HTTPStreamer>();
    this->decoder_ = make_unique<DecodeStreamer>();

    this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
    this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));

    this->mixer_ = mixer;
    this->pipeline_type_ = pipeline_type;
  }

  size_t available() { return this->decoder_->available(); }

  size_t read(uint8_t *buffer, size_t length) {
    size_t available_bytes = this->available();
    size_t bytes_to_read = std::min(length, available_bytes);
    if (bytes_to_read > 0) {
      return this->decoder_->read(buffer, bytes_to_read);
    }
    return 0;
  }

  void start(const std::string &uri, UBaseType_t priority = 1) {
    this->reader_->start(uri);
    this->decoder_->start();
    if (this->task_handle_ == nullptr) {
      xTaskCreate(Pipeline::transfer_task_, "transfer_task", 8096, (void *) this, priority, &this->task_handle_);
    }
  }

  void stop() {
    vTaskDelete(this->task_handle_);
    this->task_handle_ = nullptr;

    xQueueReset(this->event_queue_);
    xQueueReset(this->command_queue_);
  }

  BaseType_t send_command(CommandEvent *command, TickType_t ticks_to_wait = portMAX_DELAY) {
    return xQueueSend(this->command_queue_, command, ticks_to_wait);
  }

  BaseType_t read_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->event_queue_, event, ticks_to_wait);
  }

 protected:
  static void transfer_task_(void *params);

  void watch_() {
    TaskEvent event;


    while (this->reader_->read_event(&event)) {
      switch (event.type) {
        case EventType::STARTING:
          this->reading_ = true;
          break;
        case EventType::STARTED:
          this->reading_ = true;
          break;
        case EventType::IDLE:
          this->reading_ = false;
          break;
        case EventType::RUNNING:
          this->reading_ = true;
          break;
        case EventType::STOPPING:
          this->reading_ = false;
          break;
        case EventType::STOPPED: {
          this->reading_ = false;
          this->reader_->stop();
          CommandEvent command_event;
          command_event.command = CommandEventType::STOP_GRACEFULLY;
          this->decoder_->send_command(&command_event);
          break;
        }
        case EventType::WARNING:
          this->reading_ = false;
          xQueueSend(this->event_queue_, &event, portMAX_DELAY);
          break;
      }
    }

    while (this->decoder_->read_event(&event)) {
      switch (event.type) {
        case EventType::STARTING:
          this->decoding_ = true;
          break;
        case EventType::STARTED:
          this->decoding_ = true;
          break;
        case EventType::IDLE:
          this->decoding_ = false;
          break;
        case EventType::RUNNING:
          this->decoding_ = true;
          break;
        case EventType::STOPPING:
          this->decoding_ = false;
          break;
        case EventType::STOPPED:
          this->decoding_ = false;
          this->decoder_->stop();
          break;
        case EventType::WARNING:
          this->decoding_ = false;
          xQueueSend(this->event_queue_, &event, portMAX_DELAY);
          break;
      }
    }
    if (this->reading_ || this->decoding_) {
      event.type = EventType::RUNNING;
      xQueueSend(this->event_queue_, &event, portMAX_DELAY);
    } else {
      event.type = EventType::IDLE;
      xQueueSend(this->event_queue_, &event, portMAX_DELAY);
    }
  }

  bool reading_{false};
  bool decoding_{false};

  std::unique_ptr<HTTPStreamer> reader_;
  std::unique_ptr<DecodeStreamer> decoder_;
  CombineStreamer *mixer_;

  TaskHandle_t task_handle_{nullptr};

  QueueHandle_t event_queue_;
  QueueHandle_t command_queue_;

  std::string current_uri_{};
  PipelineType pipeline_type_;
};

}  // namespace nabu
}  // namespace esphome
#endif
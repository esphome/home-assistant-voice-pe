#pragma once

#ifdef USE_ESP_IDF

#include "esphome/core/ring_buffer.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include <esp_http_client.h>

#include "mp3_decoder.h"

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

enum class MediaFileType : uint8_t {
  NONE = 0,
  WAV,
  MP3,
};

struct TaskEvent {
  EventType type;
  esp_err_t err;
  MediaFileType media_file_type;
};

enum class CommandEventType : uint8_t {
  START,
  STOP,
  STOP_GRACEFULLY,
  DUCK,
  PAUSE_MEDIA,
  RESUME_MEDIA,
};

enum class PipelineType : uint8_t {
  MEDIA,
  ANNOUNCEMENT,
};

struct CommandEvent {
  CommandEventType command;
  float ducking_ratio = 0.0;
  MediaFileType media_file_type = MediaFileType::NONE;
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
  size_t read(uint8_t *buffer, size_t length, TickType_t ticks_to_wait = 0) {
    size_t available_bytes = this->available();
    size_t bytes_to_read = std::min(length, available_bytes);
    if (bytes_to_read > 0) {
      return this->output_ring_buffer_->read((void *) buffer, bytes_to_read, ticks_to_wait);
    }
    return 0;
  }

  virtual void reset_ring_buffers() { this->output_ring_buffer_->reset(); }

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

  MediaFileType establish_connection_(esp_http_client_handle_t *client);
  void cleanup_connection_(esp_http_client_handle_t *client);

  std::string current_uri_{};
};

class DecodeStreamer : public OutputStreamer {
 public:
  DecodeStreamer();
  void start(UBaseType_t priority = 1) override;
  void reset_ring_buffers() override;

  size_t input_free() { return this->input_ring_buffer_->free(); }

  bool empty() { return (this->input_ring_buffer_->available() + this->output_ring_buffer_->available()) == 0; }

  size_t write(uint8_t *buffer, size_t length);

 protected:
  static void decode_task_(void *params);
  std::unique_ptr<RingBuffer> input_ring_buffer_;

  // HMP3Decoder mp3_decoder_;
  // MP3FrameInfo mp3_frame_info_;
  // int mp3_bytes_left_ = 0;  // MP3 bytes left decode
  // uint8_t *mp3_buffer_current_ = nullptr;
  // bool mp3_printed_info_ = false;
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

}  // namespace nabu
}  // namespace esphome
#endif
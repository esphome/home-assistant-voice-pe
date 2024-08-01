#pragma once

#ifdef USE_ESP_IDF

#include "esphome/components/media_player/media_player.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include <esp_http_client.h>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace esphome {
namespace nabu {

struct StreamInfo {
  bool operator==(const StreamInfo &rhs) const {
    return (channels == rhs.channels) && (bits_per_sample == rhs.bits_per_sample) && (sample_rate == rhs.sample_rate);
  }
  bool operator!=(const StreamInfo &rhs) const { return !operator==(rhs); }
  uint8_t channels = 1;
  uint8_t bits_per_sample = 16;
  uint32_t sample_rate = 16000;
};

enum class EventType : uint8_t {
  STARTING = 0,
  STARTED,
  RUNNING,
  IDLE,
  STOPPING,
  STOPPED,
  WARNING = 255,
};

// enum class MediaFileType : uint8_t {
//   NONE = 0,
//   WAV,
//   MP3,
//   FLAC,
// };

struct TaskEvent {
  EventType type;
  esp_err_t err;
  media_player::MediaFileType media_file_type;
  StreamInfo stream_info;
};

enum class CommandEventType : uint8_t {
  START,
  STOP,
  STOP_GRACEFULLY,
  DUCK,
  PAUSE_MEDIA,
  RESUME_MEDIA,
  CLEAR_MEDIA,
  CLEAR_ANNOUNCEMENT,
};

enum class PipelineType : uint8_t {
  MEDIA,
  ANNOUNCEMENT,
};

struct CommandEvent {
  CommandEventType command;
  float ducking_ratio = 0.0;
  media_player::MediaFileType media_file_type = media_player::MediaFileType::NONE;
  StreamInfo stream_info;
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

  virtual void start(const std::string &task_name, UBaseType_t priority = 1) = 0;
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

  void start(const std::string &task_name, UBaseType_t priority = 1) override;
  void start_http(const std::string &task_name, UBaseType_t priority = 1);
  void start_file(const std::string &task_name, UBaseType_t priority = 1);
  void start(const std::string &uri, const std::string &task_name, UBaseType_t priority = 1);
  void start(media_player::MediaFile *media_file, const std::string &task_name, UBaseType_t priority = 1);

 protected:
  static void read_task_(void *params);
  static void file_read_task_(void *params);

  media_player::MediaFileType establish_connection_(esp_http_client_handle_t *client);
  void cleanup_connection_(esp_http_client_handle_t *client);

  media_player::MediaFile *current_media_file_{};
  std::string current_uri_{};
};

}  // namespace nabu
}  // namespace esphome
#endif
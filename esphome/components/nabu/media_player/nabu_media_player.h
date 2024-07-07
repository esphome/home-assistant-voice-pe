#pragma once

#ifdef USE_ESP_IDF

#include "esphome/components/media_player/media_player.h"
// #include "esphome/components/speaker/speaker.h"
#include "esphome/components/i2s_audio/speaker/i2s_audio_speaker.h"
#include "esphome/core/component.h"
#include "esphome/core/ring_buffer.h"

// #include "mp3_decoder.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

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

struct MediaCommandEvent {
  media_player::MediaPlayerCommand command;
  // bool stop;
};

struct TaskEvent {
  EventType type;
  esp_err_t err;
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
    return this->output_ring_buffer_->read((void *) buffer, bytes_to_read);
  }

  /// @brief Returns the number of bytes available to read from the ring buffer
  size_t available() { return this->output_ring_buffer_->available(); }

  BaseType_t send_command(MediaCommandEvent *command, TickType_t ticks_to_wait = portMAX_DELAY) {
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

  std::string current_uri_{};

  TaskHandle_t task_handle_{nullptr};
  std::unique_ptr<RingBuffer> output_ring_buffer_;
  QueueHandle_t event_queue_;
  QueueHandle_t command_queue_;
};

class NabuMediaPlayer : public Component, public media_player::MediaPlayer {
 public:
  float get_setup_priority() const override { return esphome::setup_priority::LATE; }
  void setup() override;
  void loop() override;

  // MediaPlayer implementations
  bool is_muted() const override { return this->muted_; }
  media_player::MediaPlayerTraits get_traits() override;

  void start() {}
  void stop() {}
  void set_speaker(i2s_audio::I2SAudioSpeaker *speaker) { this->speaker_ = speaker; }

 protected:
  // MediaPlayer implementation
  void control(const media_player::MediaPlayerCall &call) override;

  void watch_();

  bool muted_{false};
  bool play_intent_{false};

  std::unique_ptr<HTTPStreamer> media_streamer_;

  i2s_audio::I2SAudioSpeaker *speaker_{nullptr};
  bool is_playing_{false};
  uint8_t *transfer_buffer_{nullptr};

  int header_control_counter_{0};

  bool read_wav_header_(esp_http_client_handle_t *client);
  // speaker::StreamInfo stream_info_{
  //         .channels = speaker::CHANNELS_MONO, .bits_per_sample = speaker::SAMPLE_BITS_16, .sample_rate = 16000};
};

}  // namespace nabu
}  // namespace esphome

#endif

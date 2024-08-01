#pragma once

#ifdef USE_ESP_IDF

#include "streamer.h"
#include "combine_streamer.h"
#include "decode_streamer.h"
#include "resample_streamer.h"

#include "esphome/components/media_player/media_player.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace esphome {
namespace nabu {

class Pipeline {
 public:
  Pipeline(CombineStreamer *mixer, PipelineType pipeline_type);

  size_t available() { return this->decoder_->available(); }

  size_t read(uint8_t *buffer, size_t length);

  void start(const std::string &uri, const std::string &task_name, UBaseType_t priority = 1);
  void start(media_player::MediaFile *media_file, const std::string &task_name, UBaseType_t priority = 1);

  void stop();

  BaseType_t send_command(CommandEvent *command, TickType_t ticks_to_wait = portMAX_DELAY);

  BaseType_t read_event(TaskEvent *event, TickType_t ticks_to_wait = 0);

 protected:
  static void transfer_task_(void *params);
  void watch_(bool stopping_gracefully);

  std::unique_ptr<HTTPStreamer> reader_;
  std::unique_ptr<DecodeStreamer> decoder_;
  std::unique_ptr<ResampleStreamer> resampler_;
  CombineStreamer *mixer_;

  TaskHandle_t task_handle_{nullptr};

  QueueHandle_t event_queue_;
  QueueHandle_t command_queue_;

  std::string current_uri_{};
  PipelineType pipeline_type_;

  bool reading_{false};
  bool decoding_{false};
  bool resampling_{false};
};

}  // namespace nabu
}  // namespace esphome

#endif
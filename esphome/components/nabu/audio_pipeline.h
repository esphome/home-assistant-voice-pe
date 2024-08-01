#pragma once

#ifdef USE_ESP_IDF

#include "audio_reader.h"
#include "audio_decoder.h"
#include "audio_resampler.h"
#include "audio_mixer.h"

#include "esphome/components/media_player/media_player.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>



namespace esphome {
namespace nabu {

enum class AudioPipelineType : uint8_t {
  MEDIA,
  ANNOUNCEMENT,
};

enum class AudioPipelineState : uint8_t {
  STARTING,
  STARTED,
  PLAYING,
  PAUSED,
  STOPPING,
  STOPPED,
};

class AudioPipeline {
 public:
  AudioPipeline(AudioMixer *mixer, AudioPipelineType pipeline_type);

  void start(const std::string &uri, const std::string &task_name, UBaseType_t priority = 1);
  void start(media_player::MediaFile *media_file, const std::string &task_name, UBaseType_t priority = 1);

  void stop();

  AudioPipelineState get_state();

  void reset_ring_buffers();

 protected:
  void common_start_(const std::string &task_name, UBaseType_t priority);

  AudioMixer *mixer_;

  std::string current_uri_{};
  media_player::MediaFile *current_media_file_{nullptr};

  media_player::MediaFileType current_media_file_type_;
  media_player::StreamInfo current_stream_info_;

  AudioPipelineType pipeline_type_;

  std::unique_ptr<RingBuffer> raw_file_ring_buffer_;
  std::unique_ptr<RingBuffer> decoded_ring_buffer_;
  std::unique_ptr<RingBuffer> resampled_ring_buffer_;

  EventGroupHandle_t event_group_;

  static void read_task_(void *params);
  TaskHandle_t read_task_handle_{nullptr};
  StaticTask_t read_task_stack_;
  StackType_t *read_task_stack_buffer_{nullptr};

  static void decode_task_(void *params);
  TaskHandle_t decode_task_handle_{nullptr};
  StaticTask_t decode_task_stack_;
  StackType_t *decode_task_stack_buffer_{nullptr};

  static void resample_task_(void *params);
  TaskHandle_t resample_task_handle_{nullptr};
  StaticTask_t resample_task_stack_;
  StackType_t *resample_task_stack_buffer_{nullptr};
};

}  // namespace nabu
}  // namespace esphome

#endif
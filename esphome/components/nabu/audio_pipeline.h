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
  PLAYING,
  STOPPED,
  ERROR_READING,
  ERROR_DECODING,
  ERROR_RESAMPLING,
};

enum class InfoErrorSource : uint8_t {
  READER = 0,
  DECODER,
  RESAMPLER,
};

struct InfoErrorEvent {
  InfoErrorSource source;
  optional<esp_err_t> err;
  optional<media_player::MediaFileType> file_type;
  optional<media_player::StreamInfo> stream_info;
  optional<ResampleInfo> resample_info;
};

class AudioPipeline {
 public:
  AudioPipeline(AudioMixer *mixer, AudioPipelineType pipeline_type);

  esp_err_t start(const std::string &uri, uint32_t target_sample_rate, const std::string &task_name,
                  UBaseType_t priority = 1);
  esp_err_t start(media_player::MediaFile *media_file, uint32_t target_sample_rate, const std::string &task_name,
                  UBaseType_t priority = 1);

  esp_err_t stop();

  AudioPipelineState get_state();

  void reset_ring_buffers();

 protected:
  esp_err_t allocate_buffers_();
  esp_err_t common_start_(uint32_t target_sample_rate, const std::string &task_name, UBaseType_t priority);

  uint32_t target_sample_rate_;

  AudioMixer *mixer_;

  std::string current_uri_{};
  media_player::MediaFile *current_media_file_{nullptr};

  media_player::MediaFileType current_media_file_type_;
  media_player::StreamInfo current_stream_info_;
  ResampleInfo current_resample_info_;

  AudioPipelineType pipeline_type_;

  std::unique_ptr<RingBuffer> raw_file_ring_buffer_;
  std::unique_ptr<RingBuffer> decoded_ring_buffer_;
  std::unique_ptr<RingBuffer> resampled_ring_buffer_;

  // Handles basic control/state of the three tasks
  EventGroupHandle_t event_group_{nullptr};

  // Receives detailed info (file type, stream info, resampling info) or specific errors from the three tasks
  QueueHandle_t info_error_queue_{nullptr};

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
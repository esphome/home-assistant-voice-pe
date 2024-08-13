#ifdef USE_ESP_IDF

#include "audio_pipeline.h"

#include "esphome/core/helpers.h"

namespace esphome {
namespace nabu {

static const size_t QUEUE_COUNT = 10;

static const size_t HTTP_BUFFER_SIZE = 64 * 1024;
static const size_t BUFFER_SIZE_SAMPLES = 32768;
static const size_t BUFFER_SIZE_BYTES = BUFFER_SIZE_SAMPLES * sizeof(int16_t);

static const uint32_t READER_TASK_STACK_SIZE = 8192;
static const uint32_t DECODER_TASK_STACK_SIZE = 8192;
static const uint32_t RESAMPLER_TASK_STACK_SIZE = 8192;

enum EventGroupBits : uint32_t {
  PIPELINE_COMMAND_STOP = (1 << 0),  // Stops all activity in the pipeline elements

  READER_COMMAND_INIT_HTTP = (1 << 4),  // Read audio from an HTTP source
  READER_COMMAND_INIT_FILE = (1 << 5),  // Read audio from an audio file from the flash

  READER_MESSAGE_LOADED_MEDIA_TYPE = (1 << 6),  // Audio file type is read after checking it is supported
  READER_MESSAGE_FINISHED = (1 << 7),           // Reader is done (either through a failure or just end of the stream)
  READER_MESSAGE_ERROR = (1 << 8),              // Error reading the file

  DECODER_MESSAGE_LOADED_STREAM_INFO = (1 << 11),  // Decoder has determined the stream information
  DECODER_MESSAGE_FINISHED = (1 << 12),  // Decoder is done (either through a faiilure or the end of the stream)
  DECODER_MESSAGE_ERROR = (1 << 13),     // Error decoding the file

  RESAMPLER_MESSAGE_FINISHED = (1 << 17),  // Resampler is done (either through a failure or the end of the stream)
  RESAMPLER_MESSAGE_ERROR = (1 << 18),     // Error resampling the file

  ALL_BITS = 0xfffff,  // 24 total bits available in an event group
};

AudioPipeline::AudioPipeline(AudioMixer *mixer, AudioPipelineType pipeline_type) {
  this->mixer_ = mixer;
  this->pipeline_type_ = pipeline_type;
}

esp_err_t AudioPipeline::start(const std::string &uri, uint32_t target_sample_rate, const std::string &task_name,
                               UBaseType_t priority) {
  esp_err_t err = this->common_start_(target_sample_rate, task_name, priority);

  if (err == ESP_OK) {
    this->current_uri_ = uri;
    xEventGroupSetBits(this->event_group_, READER_COMMAND_INIT_HTTP);
  }

  return err;
}

esp_err_t AudioPipeline::start(media_player::MediaFile *media_file, uint32_t target_sample_rate,
                               const std::string &task_name, UBaseType_t priority) {
  esp_err_t err = this->common_start_(target_sample_rate, task_name, priority);

  if (err == ESP_OK) {
    this->current_media_file_ = media_file;
    xEventGroupSetBits(this->event_group_, READER_COMMAND_INIT_FILE);
  }

  return err;
}

esp_err_t AudioPipeline::allocate_buffers_() {
  if (this->raw_file_ring_buffer_ == nullptr)
    this->raw_file_ring_buffer_ = RingBuffer::create(HTTP_BUFFER_SIZE);

  if (this->decoded_ring_buffer_ == nullptr)
    this->decoded_ring_buffer_ = RingBuffer::create(BUFFER_SIZE_BYTES);

  if (this->resampled_ring_buffer_ == nullptr)
    this->resampled_ring_buffer_ = RingBuffer::create(BUFFER_SIZE_BYTES);

  if ((this->raw_file_ring_buffer_ == nullptr) || (this->decoded_ring_buffer_ == nullptr) ||
      (this->resampled_ring_buffer_ == nullptr)) {
    return ESP_ERR_NO_MEM;
  }

  ExternalRAMAllocator<StackType_t> stack_allocator(ExternalRAMAllocator<StackType_t>::ALLOW_FAILURE);

  if (this->read_task_stack_buffer_ == nullptr)
    this->read_task_stack_buffer_ = stack_allocator.allocate(READER_TASK_STACK_SIZE);

  if (this->decode_task_stack_buffer_ == nullptr)
    this->decode_task_stack_buffer_ = stack_allocator.allocate(DECODER_TASK_STACK_SIZE);

  if (this->resample_task_stack_buffer_ == nullptr)
    this->resample_task_stack_buffer_ = stack_allocator.allocate(RESAMPLER_TASK_STACK_SIZE);

  if ((this->read_task_stack_buffer_ == nullptr) || (this->decode_task_stack_buffer_ == nullptr) ||
      (this->resample_task_stack_buffer_ == nullptr)) {
    return ESP_ERR_NO_MEM;
  }

  if (this->event_group_ == nullptr)
    this->event_group_ = xEventGroupCreate();

  if (this->event_group_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t AudioPipeline::common_start_(uint32_t target_sample_rate, const std::string &task_name,
                                       UBaseType_t priority) {
  esp_err_t err = this->allocate_buffers_();
  if (err != ESP_OK) {
    return err;
  }

  if (this->read_task_handle_ == nullptr) {
    this->read_task_handle_ =
        xTaskCreateStatic(AudioPipeline::read_task_, (task_name + "_read").c_str(), READER_TASK_STACK_SIZE,
                          (void *) this, priority, this->read_task_stack_buffer_, &this->read_task_stack_);
  }
  if (this->decode_task_handle_ == nullptr) {
    this->decode_task_handle_ =
        xTaskCreateStatic(AudioPipeline::decode_task_, (task_name + "_decode").c_str(), DECODER_TASK_STACK_SIZE,
                          (void *) this, priority, this->decode_task_stack_buffer_, &this->decode_task_stack_);
  }
  if (this->resample_task_handle_ == nullptr) {
    this->resample_task_handle_ =
        xTaskCreateStatic(AudioPipeline::resample_task_, (task_name + "_resample").c_str(), RESAMPLER_TASK_STACK_SIZE,
                          (void *) this, priority, this->resample_task_stack_buffer_, &this->resample_task_stack_);
  }

  if ((this->read_task_handle_ == nullptr) || (this->decode_task_handle_ == nullptr) ||
      (this->resample_task_handle_ == nullptr)) {
    return ESP_FAIL;
  }

  this->stop();

  this->target_sample_rate_ = target_sample_rate;

  return ESP_OK;
}

AudioPipelineState AudioPipeline::get_state() {
  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);
  if (!this->read_task_handle_ && !this->decode_task_handle_ && !this->resample_task_handle_) {
    return AudioPipelineState::STOPPED;
  }

  if ((event_bits & READER_MESSAGE_ERROR)) {
    xEventGroupClearBits(this->event_group_, READER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_READING;
  }

  if ((event_bits & DECODER_MESSAGE_ERROR)) {
    xEventGroupClearBits(this->event_group_, DECODER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_DECODING;
  }

  if ((event_bits & RESAMPLER_MESSAGE_ERROR)) {
    xEventGroupClearBits(this->event_group_, RESAMPLER_MESSAGE_ERROR);
    return AudioPipelineState::ERROR_RESAMPLING;
  }

  if ((event_bits & READER_MESSAGE_FINISHED) && (event_bits & DECODER_MESSAGE_FINISHED) &&
      (event_bits & RESAMPLER_MESSAGE_FINISHED)) {
    return AudioPipelineState::STOPPED;
  }

  return AudioPipelineState::PLAYING;
}

void AudioPipeline::stop() {
  xEventGroupSetBits(this->event_group_, PIPELINE_COMMAND_STOP);

  xEventGroupWaitBits(
      this->event_group_,
      (READER_MESSAGE_FINISHED | DECODER_MESSAGE_FINISHED | RESAMPLER_MESSAGE_FINISHED),  // Bit message to read
      pdTRUE,                                                                             // Clear the bit on exit
      true,                                                                               // Wait for all the bits,
      pdMS_TO_TICKS(200));  // Block temporarily before deleting each task

  // Clear the ring buffer in the mixer; avoids playing incorrect audio when starting a new file while paused
  CommandEvent command_event;
  if (this->pipeline_type_ == AudioPipelineType::MEDIA) {
    command_event.command = CommandEventType::CLEAR_MEDIA;
  } else {
    command_event.command = CommandEventType::CLEAR_ANNOUNCEMENT;
  }
  this->mixer_->send_command(&command_event);

  xEventGroupClearBits(this->event_group_, ALL_BITS);
  this->reset_ring_buffers();
}

void AudioPipeline::reset_ring_buffers() {
  this->raw_file_ring_buffer_->reset();
  this->decoded_ring_buffer_->reset();
  this->resampled_ring_buffer_->reset();
}

void AudioPipeline::read_task_(void *params) {
  AudioPipeline *this_pipeline = (AudioPipeline *) params;

  while (true) {
    xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::READER_MESSAGE_FINISHED);

    // Wait until the pipeline notifies us the source of the media file
    EventBits_t event_bits =
        xEventGroupWaitBits(this_pipeline->event_group_,
                            READER_COMMAND_INIT_FILE | READER_COMMAND_INIT_HTTP,  // Bit message to read
                            pdTRUE,                                               // Clear the bit on exit
                            pdFALSE,                                              // Wait for all the bits,
                            portMAX_DELAY);                                       // Block indefinitely until bit is set

    xEventGroupClearBits(this_pipeline->event_group_, EventGroupBits::READER_MESSAGE_FINISHED);

    {
      AudioReader reader = AudioReader(this_pipeline->raw_file_ring_buffer_.get(), HTTP_BUFFER_SIZE);
      esp_err_t err = ESP_OK;
      if (event_bits & READER_COMMAND_INIT_FILE) {
        err = reader.start(this_pipeline->current_media_file_, this_pipeline->current_media_file_type_);
      } else {
        err = reader.start(this_pipeline->current_uri_, this_pipeline->current_media_file_type_);
      }
      if (err != ESP_OK) {
        // Couldn't load the file or it is an unknown type!
        xEventGroupSetBits(this_pipeline->event_group_,
                           EventGroupBits::READER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
      } else {
        // Inform the decoder that the media type is available
        xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::READER_MESSAGE_LOADED_MEDIA_TYPE);

        while (true) {
          event_bits = xEventGroupGetBits(this_pipeline->event_group_);

          if (event_bits & PIPELINE_COMMAND_STOP) {
            break;
          }

          AudioReaderState reader_state = reader.read();

          if (reader_state == AudioReaderState::FINISHED) {
            break;
          } else if (reader_state == AudioReaderState::FAILED) {
            xEventGroupSetBits(this_pipeline->event_group_,
                               EventGroupBits::READER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
            break;
          }

          // Block to give other tasks opportunity to run
          delay(10);
        }
      }
    }
  }
}

void AudioPipeline::decode_task_(void *params) {
  AudioPipeline *this_pipeline = (AudioPipeline *) params;

  while (true) {
    xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::DECODER_MESSAGE_FINISHED);

    // Wait until the reader notifies us that the media type is available
    EventBits_t event_bits = xEventGroupWaitBits(this_pipeline->event_group_,
                                                 READER_MESSAGE_LOADED_MEDIA_TYPE,  // Bit message to read
                                                 pdTRUE,                            // Clear the bit on exit
                                                 pdFALSE,                           // Wait for all the bits,
                                                 portMAX_DELAY);  // Block indefinitely until bit is set

    xEventGroupClearBits(this_pipeline->event_group_, EventGroupBits::DECODER_MESSAGE_FINISHED);

    {
      AudioDecoder decoder = AudioDecoder(this_pipeline->raw_file_ring_buffer_.get(),
                                          this_pipeline->decoded_ring_buffer_.get(), HTTP_BUFFER_SIZE);
      esp_err_t err = decoder.start(this_pipeline->current_media_file_type_);

      if (err != ESP_OK) {
        // Setting up the decoder failed
        xEventGroupSetBits(this_pipeline->event_group_,
                           EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
      }

      bool has_stream_info = false;

      while (true) {
        event_bits = xEventGroupGetBits(this_pipeline->event_group_);

        if (event_bits & PIPELINE_COMMAND_STOP) {
          break;
        }

        // Stop gracefully if the reader has finished
        AudioDecoderState decoder_state = decoder.decode(event_bits & READER_MESSAGE_FINISHED);

        if (decoder_state == AudioDecoderState::FINISHED) {
          break;
        } else if (decoder_state == AudioDecoderState::FAILED) {
          xEventGroupSetBits(this_pipeline->event_group_,
                             EventGroupBits::DECODER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
          break;
        }

        if (!has_stream_info && decoder.get_stream_info().has_value()) {
          has_stream_info = true;

          this_pipeline->current_stream_info_ = decoder.get_stream_info().value();

          // Inform the resampler that the stream information is available
          xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::DECODER_MESSAGE_LOADED_STREAM_INFO);
        }

        // Block to give other tasks opportunity to run
        delay(10);
      }
    }
  }
}

void AudioPipeline::resample_task_(void *params) {
  AudioPipeline *this_pipeline = (AudioPipeline *) params;

  while (true) {
    xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::RESAMPLER_MESSAGE_FINISHED);

    // Wait until the decoder notifies us that the stream information is available
    EventBits_t event_bits = xEventGroupWaitBits(this_pipeline->event_group_,
                                                 DECODER_MESSAGE_LOADED_STREAM_INFO,  // Bit message to read
                                                 pdTRUE,                              // Clear the bit on exit
                                                 pdFALSE,                             // Wait for all the bits,
                                                 portMAX_DELAY);  // Block indefinitely until bit is set

    xEventGroupClearBits(this_pipeline->event_group_, EventGroupBits::RESAMPLER_MESSAGE_FINISHED);

    {
      RingBuffer *output_ring_buffer = nullptr;

      if (this_pipeline->pipeline_type_ == AudioPipelineType::MEDIA) {
        output_ring_buffer = this_pipeline->mixer_->get_media_ring_buffer();
      } else {
        output_ring_buffer = this_pipeline->mixer_->get_announcement_ring_buffer();
      }

      AudioResampler resampler =
          AudioResampler(this_pipeline->decoded_ring_buffer_.get(), output_ring_buffer, BUFFER_SIZE_SAMPLES);

      esp_err_t err = resampler.start(this_pipeline->current_stream_info_, this_pipeline->target_sample_rate_);

      if (err != ESP_OK) {
        // Unsupported incoming audio stream or other failure
        xEventGroupSetBits(this_pipeline->event_group_,
                           EventGroupBits::RESAMPLER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
      }

      while (true) {
        event_bits = xEventGroupGetBits(this_pipeline->event_group_);

        if (event_bits & PIPELINE_COMMAND_STOP) {
          break;
        }

        // Stop gracefully if the decoder is done
        AudioResamplerState resampler_state = resampler.resample(event_bits & DECODER_MESSAGE_FINISHED);

        if (resampler_state == AudioResamplerState::FINISHED) {
          break;
        } else if (resampler_state == AudioResamplerState::FAILED) {
          xEventGroupSetBits(this_pipeline->event_group_,
                             EventGroupBits::RESAMPLER_MESSAGE_ERROR | EventGroupBits::PIPELINE_COMMAND_STOP);
          break;
        }

        // Block to give other tasks opportunity to run
        delay(10);
      }
    }
  }
}

}  // namespace nabu
}  // namespace esphome
#endif
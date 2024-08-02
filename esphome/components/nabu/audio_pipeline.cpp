#ifdef USE_ESP_IDF

#include "audio_pipeline.h"

#include "esphome/core/helpers.h"

namespace esphome {
namespace nabu {

static const size_t QUEUE_COUNT = 10;

static const size_t HTTP_BUFFER_SIZE = 128 * 1024;
static const size_t BUFFER_SIZE_SAMPLES = 32768;
static const size_t BUFFER_SIZE_BYTES = BUFFER_SIZE_SAMPLES * sizeof(int16_t);

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
  this->raw_file_ring_buffer_ = RingBuffer::create(HTTP_BUFFER_SIZE);
  this->decoded_ring_buffer_ = RingBuffer::create(BUFFER_SIZE_BYTES);
  this->resampled_ring_buffer_ = RingBuffer::create(BUFFER_SIZE_BYTES);

  this->mixer_ = mixer;
  this->pipeline_type_ = pipeline_type;

  ExternalRAMAllocator<StackType_t> allocator(ExternalRAMAllocator<StackType_t>::ALLOW_FAILURE);

  this->read_task_stack_buffer_ = allocator.allocate(8192);
  this->decode_task_stack_buffer_ = allocator.allocate(8192);
  this->resample_task_stack_buffer_ = allocator.allocate(8192);

  this->event_group_ = xEventGroupCreate();
}

void AudioPipeline::start(const std::string &uri, uint32_t target_sample_rate, const std::string &task_name, UBaseType_t priority) {
  this->common_start_(target_sample_rate, task_name, priority);

  this->current_uri_ = uri;
  xEventGroupSetBits(this->event_group_, READER_COMMAND_INIT_HTTP);
}

void AudioPipeline::start(media_player::MediaFile *media_file, uint32_t target_sample_rate, const std::string &task_name, UBaseType_t priority) {
  this->common_start_(target_sample_rate, task_name, priority);

  this->current_media_file_ = media_file;
  xEventGroupSetBits(this->event_group_, READER_COMMAND_INIT_FILE);
}

void AudioPipeline::common_start_(uint32_t target_sample_rate, const std::string &task_name, UBaseType_t priority) {
  if (this->read_task_handle_ == nullptr) {
    this->read_task_handle_ =
        xTaskCreateStatic(AudioPipeline::read_task_, (task_name + "_read").c_str(), 8192, (void *) this, priority,
                          this->read_task_stack_buffer_, &this->read_task_stack_);
  }
  if (this->decode_task_handle_ == nullptr) {
    this->decode_task_handle_ =
        xTaskCreateStatic(AudioPipeline::decode_task_, (task_name + "_decode").c_str(), 8192, (void *) this, priority,
                          this->decode_task_stack_buffer_, &this->decode_task_stack_);
  }
  if (this->resample_task_handle_ == nullptr) {
    this->resample_task_handle_ =
        xTaskCreateStatic(AudioPipeline::resample_task_, (task_name + "_resample").c_str(), 8192, (void *) this,
                          priority, this->resample_task_stack_buffer_, &this->resample_task_stack_);
  }

  this->stop();

  this->target_sample_rate_ = target_sample_rate;
}

AudioPipelineState AudioPipeline::get_state() {
  EventBits_t event_bits = xEventGroupGetBits(this->event_group_);
  if (!this->read_task_handle_ && !this->decode_task_handle_ && !this->resample_task_handle_) {
    return AudioPipelineState::STOPPED;
  } else if ((event_bits & READER_MESSAGE_FINISHED) && (event_bits & DECODER_MESSAGE_FINISHED) &&
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
      if (event_bits & READER_COMMAND_INIT_FILE) {
        this_pipeline->current_media_file_type_ = reader.start(this_pipeline->current_media_file_);
      } else {
        this_pipeline->current_media_file_type_ = reader.start(this_pipeline->current_uri_);
      }
      if (this_pipeline->current_media_file_type_ == media_player::MediaFileType::NONE) {
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
                                          this_pipeline->decoded_ring_buffer_.get(), HTTP_BUFFER_SIZE);//BUFFER_SIZE_BYTES);
      decoder.start(this_pipeline->current_media_file_type_);

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

        if (!has_stream_info && decoder.get_channels().has_value()) {
          has_stream_info = true;

          this_pipeline->current_stream_info_.channels = decoder.get_channels().value();
          this_pipeline->current_stream_info_.bits_per_sample = decoder.get_sample_depth().value();
          this_pipeline->current_stream_info_.sample_rate = decoder.get_sample_rate().value();

          // Inform the resampler that the stream information is available
          xEventGroupSetBits(this_pipeline->event_group_, EventGroupBits::DECODER_MESSAGE_LOADED_STREAM_INFO);
        }

        // Block to give other tasks opportunity to run
        delay(15);
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

      resampler.start(this_pipeline->current_stream_info_, this_pipeline->target_sample_rate_);

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
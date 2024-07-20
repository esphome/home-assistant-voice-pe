#ifdef USE_ESP_IDF

#include "resample_streamer.h"

#include "biquad.h"
#include "resampler.h"
#include "streamer.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace nabu {

static const size_t BUFFER_SIZE = 4096;
static const size_t QUEUE_COUNT = 20;

static const size_t NUM_TAPS = 32;
static const size_t NUM_FILTERS = 32;
static const bool USE_PRE_POST_FILTER = false;

ResampleStreamer::ResampleStreamer() {
  this->input_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  this->output_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));

  // TODO: Handle if this fails to allocate
  if ((this->input_ring_buffer_) || (this->output_ring_buffer_ == nullptr)) {
    return;
  }
}

void ResampleStreamer::start(const std::string &task_name, UBaseType_t priority) {
  if (this->task_handle_ == nullptr) {
    xTaskCreate(ResampleStreamer::resample_task_, task_name.c_str(), 3072, (void *) this, priority,
                &this->task_handle_);
  }
}

size_t ResampleStreamer::write(uint8_t *buffer, size_t length) {
  size_t free_bytes = this->input_ring_buffer_->free();
  size_t bytes_to_write = std::min(length, free_bytes);
  if (bytes_to_write > 0) {
    return this->input_ring_buffer_->write((void *) buffer, bytes_to_write);
  }
  return 0;
}

void ResampleStreamer::resample_task_(void *params) {
  ResampleStreamer *this_streamer = (ResampleStreamer *) params;

  TaskEvent event;
  CommandEvent command_event;

  ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  int16_t *input_buffer = allocator.allocate(BUFFER_SIZE);
  int16_t *output_buffer = allocator.allocate(BUFFER_SIZE);

  ExternalRAMAllocator<float> float_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);
  float *float_input_buffer = float_allocator.allocate(BUFFER_SIZE);
  float *float_output_buffer = float_allocator.allocate(BUFFER_SIZE);

  int16_t *input_buffer_current = input_buffer;
  int16_t *output_buffer_current = output_buffer;

  size_t input_buffer_length = 0;
  size_t output_buffer_length = 0;

  if ((input_buffer == nullptr) || (output_buffer == nullptr)) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  MediaFileType media_file_type = MediaFileType::NONE;
  StreamInfo stream_info;
  stream_info.channels = 0;  // will be set once we receive the start command

  bool resample = false;
  Resample *resampler = nullptr;
  float sample_ratio = 1.0;
  float lowpass_ratio = 1.0;
  int flags = 0;
  Biquad lowpass[2][2];
  BiquadCoefficients lowpass_coeff;


  bool pre_filter = false;
  bool post_filter = false;

  bool stopping = false;

  uint8_t channel_factor = 1;

  while (true) {
    if (xQueueReceive(this_streamer->command_queue_, &command_event, (0 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::START) {
        // Need to receive stream information here
        media_file_type = command_event.media_file_type;
        stream_info = command_event.stream_info;

        if (stream_info.channels > 0) {
          constexpr uint8_t output_channels = 2;  // fix to stereo output for now
          channel_factor = output_channels / stream_info.channels;
        }
        float resample_rate = 16000.0f;
        if (stream_info.sample_rate != 16000) {
          resample = true;
          sample_ratio = resample_rate / stream_info.sample_rate;
          if (sample_ratio < 1.0) {
            lowpass_ratio -= (10.24 / 16);

            if (lowpass_ratio < 0.84) {
              lowpass_ratio = 0.84;
            }

            if (lowpass_ratio < sample_ratio) {
              // avoid discontinuities near unity sample ratios
              lowpass_ratio = sample_ratio;
            }
          }
          if (lowpass_ratio * sample_ratio < 0.98 && USE_PRE_POST_FILTER) {
            double cutoff = lowpass_ratio * sample_ratio / 2.0;
            biquad_lowpass(&lowpass_coeff, cutoff);
            pre_filter = true;
          }

          if (lowpass_ratio / sample_ratio < 0.98 && USE_PRE_POST_FILTER && !pre_filter) {
            double cutoff = lowpass_ratio / sample_ratio / 2.0;
            biquad_lowpass(&lowpass_coeff, cutoff);
            post_filter = true;
          }

          if (pre_filter || post_filter) {
            for (int i = 0; i < stream_info.channels; ++i) {
              biquad_init(&lowpass[i][0], &lowpass_coeff, 1.0);
              biquad_init(&lowpass[i][1], &lowpass_coeff, 1.0);
            }
          }

          if (sample_ratio < 1.0) {
            resampler = resampleInit(stream_info.channels, NUM_TAPS, NUM_FILTERS, sample_ratio * lowpass_ratio,
                                     flags | INCLUDE_LOWPASS);
          } else if (lowpass_ratio < 1.0) {
            resampler =
                resampleInit(stream_info.channels, NUM_TAPS, NUM_FILTERS, lowpass_ratio, flags | INCLUDE_LOWPASS);
          } else {
            resampler = resampleInit(stream_info.channels, NUM_TAPS, NUM_FILTERS, 1.0, flags);
          }

          resampleAdvancePosition(resampler, NUM_TAPS / 2.0);
        } else {
          resample = false;
        }

        this_streamer->reset_ring_buffers();

        input_buffer_current = input_buffer;
        output_buffer_current = output_buffer;
        input_buffer_length = 0;   // measured in bytes
        output_buffer_length = 0;  // measured in bytes
      } else if (command_event.command == CommandEventType::STOP) {
        break;
      } else if (command_event.command == CommandEventType::STOP_GRACEFULLY) {
        stopping = true;
      }
    }

    if (output_buffer_length > 0) {
      // If we have data in the internal output buffer, only work on writing it to the ring buffer

      size_t bytes_to_write = std::min(output_buffer_length, this_streamer->output_ring_buffer_->free());
      size_t bytes_written = 0;
      if (bytes_to_write > 0) {
        bytes_written = this_streamer->output_ring_buffer_->write((void *) output_buffer_current, bytes_to_write);

        output_buffer_current += bytes_written / sizeof(int16_t);
        output_buffer_length -= bytes_written;
      }
    } else {
      //////
      // Refill input buffer
      //////

      // Move old data to the start of the buffer
      if (input_buffer_length > 0) {
        memmove((void *) input_buffer, (void *) input_buffer_current, input_buffer_length);
      }
      input_buffer_current = input_buffer;

      // Copy new data to the end of the of the buffer
      size_t bytes_available = this_streamer->input_ring_buffer_->available();
      size_t bytes_to_read = std::min(bytes_available, BUFFER_SIZE * sizeof(int16_t) - input_buffer_length);

      if (bytes_to_read > 0) {
        int16_t *new_input_buffer_data = input_buffer + input_buffer_length / sizeof(int16_t);
        size_t bytes_read = this_streamer->input_ring_buffer_->read((void *) new_input_buffer_data, bytes_to_read,
                                                                    (10 / portTICK_PERIOD_MS));

        input_buffer_length += bytes_read;
      }

      /////
      // Resample here
      /////

      if (resample) {
        if (input_buffer_length > 0) {
          // Samples are indiviudal int16 values. Frames include 1 sample for mono and 2 samples for stereo
          // Be careful converting between bytes, samples, and frames!
          // 1 sample = 2 bytes = sizeof(int16_t)
          // if mono:
          //    1 frame = 1 sample
          // if stereo:
          //    1 frame = 2 samples (left and right)

          size_t samples_read = input_buffer_length / sizeof(int16_t);

          // This is inefficient! It reconverts any samples that weren't used in the previous resampling run
          for (int i = 0; i < samples_read; ++i) {
            float_input_buffer[i] = static_cast<float>(input_buffer[i]) / 32768.0f;
          }

          size_t frames_read = samples_read / stream_info.channels;

          // The low pass filter seems to be causing glitches... probably because samples are repeated due to the above ineffeciency!
          if (pre_filter) {
            for (int i = 0; i < stream_info.channels; ++i) {
              biquad_apply_buffer(&lowpass[i][0], float_input_buffer + i, frames_read, stream_info.channels);
              biquad_apply_buffer(&lowpass[i][1], float_input_buffer + i, frames_read, stream_info.channels);
            }
          }

          ResampleResult res;

          res = resampleProcessInterleaved(resampler, float_input_buffer, frames_read, float_output_buffer,
                                           BUFFER_SIZE / channel_factor, sample_ratio);

          size_t frames_used = res.input_used;
          size_t samples_used = frames_used * stream_info.channels;

          size_t frames_generated = res.output_generated;
          if (post_filter) {
            for (int i = 0; i < stream_info.channels; ++i) {
              biquad_apply_buffer(&lowpass[i][0], float_output_buffer + i, frames_generated, stream_info.channels);
              biquad_apply_buffer(&lowpass[i][1], float_output_buffer + i, frames_generated, stream_info.channels);
            }
          }

          size_t samples_generated = frames_generated * stream_info.channels;

          for (int i = 0; i < samples_generated; ++i) {
            output_buffer[i] = static_cast<int16_t>(float_output_buffer[i] * 32767);
          }

          input_buffer_current += samples_used;
          input_buffer_length -= samples_used * sizeof(int16_t);

          output_buffer_current = output_buffer;
          output_buffer_length += samples_generated * sizeof(int16_t);
        }

      } else {
        size_t bytes_to_transfer = std::min(BUFFER_SIZE * sizeof(int16_t) / channel_factor, input_buffer_length);
        std::memcpy((void *) output_buffer, (void *) input_buffer_current, bytes_to_transfer);

        input_buffer_current += bytes_to_transfer / sizeof(int16_t);
        input_buffer_length -= bytes_to_transfer;

        output_buffer_current = output_buffer;
        output_buffer_length += bytes_to_transfer;
      }

      if (stream_info.channels == 1) {
        // Convert mono to stereo
        for (int i = output_buffer_length / (sizeof(int16_t)) - 1; i >= 0; --i) {
          output_buffer[2 * i] = output_buffer[i];
          output_buffer[2 * i + 1] = output_buffer[i];
        }

        output_buffer_length *= 2;  // double the bytes for stereo samples
      }
    }

    if (this_streamer->input_ring_buffer_->available() || this_streamer->output_ring_buffer_->available() ||
        (output_buffer_length > 0) || (input_buffer_length > 0)) {
      event.type = EventType::RUNNING;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else if (stopping) {
      break;
    } else {
      event.type = EventType::IDLE;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    }
  }
  event.type = EventType::STOPPING;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  this_streamer->reset_ring_buffers();
  allocator.deallocate(input_buffer, BUFFER_SIZE);
  allocator.deallocate(output_buffer, BUFFER_SIZE);
  float_allocator.deallocate(float_input_buffer, BUFFER_SIZE);
  float_allocator.deallocate(float_output_buffer, BUFFER_SIZE);

  if (resampler != nullptr) {
    resampleFree(resampler);
  }

  event.type = EventType::STOPPED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void ResampleStreamer::reset_ring_buffers() {
  this->input_ring_buffer_->reset();
  this->output_ring_buffer_->reset();
}

}  // namespace nabu
}  // namespace esphome
#endif
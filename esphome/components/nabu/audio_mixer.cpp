#ifdef USE_ESP_IDF

#include "audio_mixer.h"

#include "esp_dsp.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace nabu {

static const size_t INPUT_RING_BUFFER_SIZE = 32768;  // Audio samples
static const size_t BUFFER_SIZE = 9600;              // Audio samples - keep small for fast pausing
static const size_t QUEUE_COUNT = 20;

static const uint32_t TASK_STACK_SIZE = 3072;
static const size_t DURATION_TASK_DELAY_MS = 20;

// Gives the Q15 fixed point scaling factor to reduce by 0 dB, 1dB, ..., 50 dB
// dB to PCM scaling factor formula: floating_point_scale_factor = 2^(-db/6.014)
// float to Q15 fixed point formula: q15_scale_factor = floating_point_scale_factor * 2^(15)
static const std::vector<int16_t> decibel_reduction_q15_table = {
    32767, 29201, 26022, 23189, 20665, 18415, 16410, 14624, 13032, 11613, 10349, 9222, 8218, 7324, 6527, 5816, 5183,
    4619,  4116,  3668,  3269,  2913,  2596,  2313,  2061,  1837,  1637,  1459,  1300, 1158, 1032, 920,  820,  731,
    651,   580,   517,   461,   411,   366,   326,   291,   259,   231,   206,   183,  163,  146,  130,  116,  103};

size_t AudioMixer::write_media(uint8_t *buffer, size_t length) {
  size_t free_bytes = this->media_free();
  size_t bytes_to_write = std::min(length, free_bytes);
  if (bytes_to_write > 0) {
    return this->media_ring_buffer_->write((void *) buffer, bytes_to_write);
  }
  return 0;
}

size_t AudioMixer::write_announcement(uint8_t *buffer, size_t length) {
  size_t free_bytes = this->announcement_free();
  size_t bytes_to_write = std::min(length, free_bytes);

  if (bytes_to_write > 0) {
    return this->announcement_ring_buffer_->write((void *) buffer, bytes_to_write);
  }
  return 0;
}

esp_err_t AudioMixer::allocate_buffers_() {
  if (this->media_ring_buffer_ == nullptr)
    this->media_ring_buffer_ = RingBuffer::create(INPUT_RING_BUFFER_SIZE);

  if (this->announcement_ring_buffer_ == nullptr)
    this->announcement_ring_buffer_ = RingBuffer::create(INPUT_RING_BUFFER_SIZE);

  if (this->output_ring_buffer_ == nullptr)
    this->output_ring_buffer_ = RingBuffer::create(BUFFER_SIZE);

  if ((this->output_ring_buffer_ == nullptr) || (this->media_ring_buffer_ == nullptr) ||
      ((this->output_ring_buffer_ == nullptr))) {
    return ESP_ERR_NO_MEM;
  }

  ExternalRAMAllocator<StackType_t> stack_allocator(ExternalRAMAllocator<StackType_t>::ALLOW_FAILURE);

  if (this->stack_buffer_ == nullptr)
    this->stack_buffer_ = stack_allocator.allocate(TASK_STACK_SIZE);

  if (this->stack_buffer_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  if (this->event_queue_ == nullptr)
    this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));

  if (this->command_queue_ == nullptr)
    this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));

  if ((this->event_queue_ == nullptr) || (this->command_queue_ == nullptr)) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t AudioMixer::start(const std::string &task_name, UBaseType_t priority) {
  esp_err_t err = this->allocate_buffers_();

  if (err != ESP_OK) {
    return err;
  }

  if (this->task_handle_ == nullptr) {
    this->task_handle_ = xTaskCreateStatic(AudioMixer::mix_task_, task_name.c_str(), TASK_STACK_SIZE, (void *) this,
                                           priority, this->stack_buffer_, &this->task_stack_);
  }

  if (this->task_handle_ == nullptr) {
    return ESP_FAIL;
  }

  return ESP_OK;
}

void AudioMixer::reset_ring_buffers() {
  this->output_ring_buffer_->reset();
  this->media_ring_buffer_->reset();
  this->announcement_ring_buffer_->reset();
}

void AudioMixer::mix_task_(void *params) {
  AudioMixer *this_mixer = (AudioMixer *) params;

  TaskEvent event;
  CommandEvent command_event;

  ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  int16_t *media_buffer = allocator.allocate(BUFFER_SIZE);
  int16_t *announcement_buffer = allocator.allocate(BUFFER_SIZE);
  int16_t *combination_buffer = allocator.allocate(BUFFER_SIZE);

  if ((media_buffer == nullptr) || (announcement_buffer == nullptr)) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

  // Handles media stream pausing
  bool transfer_media = true;

  // Parameters to control the ducking dB reduction and its transitions
  uint8_t target_ducking_db_reduction = 0;
  uint8_t current_ducking_db_reduction = 0;

  // Each step represents a change in 1 dB. Positive 1 means the dB reduction is increasing. Negative 1 means the dB
  // reduction is decreasing.
  int8_t db_change_per_ducking_step = 1;

  size_t ducking_transition_samples_remaining = 0;
  size_t samples_per_ducking_step = 0;

  while (true) {
    if (xQueueReceive(this_mixer->command_queue_, &command_event, pdMS_TO_TICKS(DURATION_TASK_DELAY_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::STOP) {
        break;
      } else if (command_event.command == CommandEventType::DUCK) {
        if (target_ducking_db_reduction != command_event.decibel_reduction) {
          current_ducking_db_reduction = target_ducking_db_reduction;

          target_ducking_db_reduction = command_event.decibel_reduction;
          ducking_transition_samples_remaining = command_event.transition_samples;

          uint8_t total_ducking_steps = 0;
          if (target_ducking_db_reduction > current_ducking_db_reduction) {
            // The dB reduction level is increasing (which results in quiter audio)
            total_ducking_steps = target_ducking_db_reduction - current_ducking_db_reduction;
            db_change_per_ducking_step = 1;
          } else {
            // The dB reduction level is decreasing (which results in louder audio)
            total_ducking_steps = current_ducking_db_reduction - target_ducking_db_reduction;
            db_change_per_ducking_step = -1;
          }

          samples_per_ducking_step = ducking_transition_samples_remaining / total_ducking_steps;
        }
      } else if (command_event.command == CommandEventType::PAUSE_MEDIA) {
        transfer_media = false;
      } else if (command_event.command == CommandEventType::RESUME_MEDIA) {
        transfer_media = true;
      } else if (command_event.command == CommandEventType::CLEAR_MEDIA) {
        ducking_transition_samples_remaining = 0;  // Reset ducking to the target level
        this_mixer->media_ring_buffer_->reset();
      } else if (command_event.command == CommandEventType::CLEAR_ANNOUNCEMENT) {
        this_mixer->announcement_ring_buffer_->reset();
      }
    }

    size_t media_available = this_mixer->media_ring_buffer_->available();
    size_t announcement_available = this_mixer->announcement_ring_buffer_->available();
    size_t output_free = this_mixer->output_ring_buffer_->free();

    if ((output_free > 0) && (media_available * transfer_media + announcement_available > 0)) {
      size_t bytes_to_read = std::min(output_free, BUFFER_SIZE);

      if (media_available * transfer_media > 0) {
        bytes_to_read = std::min(bytes_to_read, media_available);
      }

      if (announcement_available > 0) {
        bytes_to_read = std::min(bytes_to_read, announcement_available);
      }

      if (bytes_to_read > 0) {
        size_t media_bytes_read = 0;
        if (media_available * transfer_media > 0) {
          media_bytes_read = this_mixer->media_ring_buffer_->read((void *) media_buffer, bytes_to_read, 0);
          if (media_bytes_read > 0) {
            size_t samples_read = media_bytes_read / sizeof(int16_t);
            if (ducking_transition_samples_remaining > 0) {
              // Ducking level is still transitioning

              size_t samples_left = ducking_transition_samples_remaining;

              int16_t *current_media_buffer = media_buffer;
              int16_t *current_combination_buffer = combination_buffer;

              size_t samples_left_in_step = samples_left % samples_per_ducking_step;
              if (samples_left_in_step == 0) {
                // Start of a new step
                current_ducking_db_reduction += db_change_per_ducking_step;
                samples_left_in_step = samples_per_ducking_step;
              }
              size_t samples_left_to_duck = std::min(samples_left_in_step, samples_read);

              while (samples_left_to_duck > 0) {
                uint8_t safe_db_reduction_index =
                    clamp<uint8_t>(current_ducking_db_reduction, 0, decibel_reduction_q15_table.size() - 1);

#if defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32)
                dsps_mulc_s16_ae32(current_media_buffer, current_combination_buffer, samples_left_to_duck,
                                   decibel_reduction_q15_table[safe_db_reduction_index], 1, 1);
#else
                dsps_mulc_s16_ansi(current_media_buffer, current_combination_buffer, samples_left_to_duck,
                                   decibel_reduction_q15_table[safe_db_reduction_index], 1, 1);
#endif
                current_media_buffer += samples_left_to_duck;
                current_combination_buffer += samples_left_to_duck;

                samples_read -= samples_left_to_duck;
                samples_left -= samples_left_to_duck;

                samples_left_in_step = samples_left % samples_per_ducking_step;
                if (samples_left_in_step == 0) {
                  // Start of a new step
                  current_ducking_db_reduction += db_change_per_ducking_step;
                  samples_left_in_step = samples_per_ducking_step;
                }
                samples_left_to_duck = std::min(samples_left_in_step, samples_read);
              }

              std::memcpy((void *) media_buffer, (void *) combination_buffer, media_bytes_read);
            } else if (target_ducking_db_reduction > 0) {
              // Ducking reduction, but we are done transitioning
              uint8_t safe_db_reduction_index =
                  clamp<uint8_t>(target_ducking_db_reduction, 0, decibel_reduction_q15_table.size() - 1);

#if defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32)
              dsps_mulc_s16_ae32(media_buffer, combination_buffer, samples_read,
                                 decibel_reduction_q15_table[safe_db_reduction_index], 1, 1);
#else
              dsps_mulc_s16_ansi(media_buffer, combination_buffer, samples_read,
                                 decibel_reduction_q15_table[safe_db_reduction_index], 1, 1);
#endif
              std::memcpy((void *) media_buffer, (void *) combination_buffer, media_bytes_read);
            }
          }
        }

        size_t announcement_bytes_read = 0;
        if (announcement_available > 0) {
          announcement_bytes_read =
              this_mixer->announcement_ring_buffer_->read((void *) announcement_buffer, bytes_to_read, 0);
        }

        size_t bytes_written = 0;
        if ((media_bytes_read > 0) && (announcement_bytes_read > 0)) {
          // We have both a media and an announcement stream, so mix them together

          size_t samples_read = bytes_to_read / sizeof(int16_t);

          // We first test adding the two clips samples together and check for any clipping
          // We want the announcement volume to be consistent, regardless if media is playing or not
          // If there is clipping, we determine what factor we need to multiply that media sample by to avoid it
          // We take the smallest factor necessary for all the samples so the media volume is consistent on this batch
          // of samples
          // Note: This may not be the best approach. Adding 2 audio samples together makes both sound louder, even if
          // we are not clipping. As a result, the mixed announcement will sound louder (by around 3dB if the audio
          // streams are independent?) than if it were by itself.
          int16_t q15_scaling_factor = INT16_MAX;
          for (int i = 0; i < samples_read; ++i) {
            int32_t added_sample = static_cast<int32_t>(media_buffer[i]) + static_cast<int32_t>(announcement_buffer[i]);

            if ((added_sample > INT16_MAX) || (added_sample < INT16_MIN)) {
              // This is the largest magnitude the media sample can be to avoid clipping (converted to Q30 fixed point)
              int32_t q30_media_sample_safe_max = static_cast<int32_t>(INT16_MAX - std::abs(announcement_buffer[i]))
                                                  << 15;

              // This is the actual media sample value (converted to Q30 fixed point)
              int32_t media_sample_value = media_buffer[i];

              // This is calculation performs the Q15 division for media_sample_safe_max/media_sample_value
              // Reference: https://sestevenson.wordpress.com/2010/09/20/fixed-point-division-2/ (accessed August 15,
              // 2024)
              int16_t necessary_q15_factor = static_cast<int16_t>(q30_media_sample_safe_max / media_sample_value);
              // Take the minimum scaling factor (the smaller the factor, the more it needs to be scaled down)
              q15_scaling_factor = std::min(necessary_q15_factor, q15_scaling_factor);
            } else {
              // Store the combined samples in the combination buffer. If we do not need to scale, then we will already
              // be done after the loop finishes
              combination_buffer[i] = added_sample;
            }
          }

          if (q15_scaling_factor < INT16_MAX) {
            // Need to scale to avoid clipping

            // Scale the media samples and temporarily store them in the combination buffer
#if defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32)
            dsps_mulc_s16_ae32(media_buffer, combination_buffer, samples_read, q15_scaling_factor, 1, 1);
#else
            dsps_mulc_s16_ansi(media_buffer, combination_buffer, samples_read, q15_scaling_factor, 1, 1);
#endif

            // Move the scaled media samples back into the media befure
            std::memcpy((void *) media_buffer, (void *) combination_buffer, media_bytes_read);

            // Add together both streams, with no bitshift
            // (input buffer 1, input buffer 2, output buffer, length, input buffer 1 step, input buffer 2 step, output
            // buffer step, bitshift)

#if defined(USE_ESP32_VARIANT_ESP32S3)
            dsps_add_s16_aes3(media_buffer, announcement_buffer, combination_buffer, samples_read, 1, 1, 1, 0);
#elif defined(USE_ESP32_VARIANT_ESP32)
            dsps_add_s16_ae32(media_buffer, announcement_buffer, combination_buffer, samples_read, 1, 1, 1, 0);
#else
            dsps_add_s16_ansi(media_buffer, announcement_buffer, combination_buffer, samples_read, 1, 1, 1, 0);
#endif
          }

          bytes_written = this_mixer->output_ring_buffer_->write((void *) combination_buffer, bytes_to_read);
        } else if (media_bytes_read > 0) {
          bytes_written = this_mixer->output_ring_buffer_->write((void *) media_buffer, media_bytes_read);

        } else if (announcement_bytes_read > 0) {
          bytes_written = this_mixer->output_ring_buffer_->write((void *) announcement_buffer, announcement_bytes_read);
        }

        size_t samples_written = bytes_written / sizeof(int16_t);
        if (ducking_transition_samples_remaining > 0) {
          ducking_transition_samples_remaining -= std::min(samples_written, ducking_transition_samples_remaining);
        }
      }
    }
  }

  event.type = EventType::STOPPING;
  xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

  this_mixer->reset_ring_buffers();
  allocator.deallocate(media_buffer, BUFFER_SIZE);
  allocator.deallocate(announcement_buffer, BUFFER_SIZE);
  allocator.deallocate(combination_buffer, BUFFER_SIZE);

  event.type = EventType::STOPPED;
  xQueueSend(this_mixer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

}  // namespace nabu
}  // namespace esphome
#endif
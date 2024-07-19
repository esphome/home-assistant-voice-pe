#ifdef USE_ESP_IDF

#include "combine_streamer.h"

#include "esp_dsp.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace nabu {

static const size_t BUFFER_SIZE = 4*2048;
static const size_t QUEUE_COUNT = 10;

CombineStreamer::CombineStreamer() {
  this->output_ring_buffer_ = RingBuffer::create(BUFFER_SIZE);
  this->media_ring_buffer_ = RingBuffer::create(BUFFER_SIZE);
  this->announcement_ring_buffer_ = RingBuffer::create(BUFFER_SIZE);

  // TODO: Handle not being able to allocate these...
  if ((this->output_ring_buffer_ == nullptr) || (this->media_ring_buffer_ == nullptr) ||
      ((this->output_ring_buffer_ == nullptr))) {
    return;
  }

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));
}

size_t CombineStreamer::write_media(uint8_t *buffer, size_t length) {
  size_t free_bytes = this->media_free();
  size_t bytes_to_write = std::min(length, free_bytes);
  if (bytes_to_write > 0) {
    return this->media_ring_buffer_->write((void *) buffer, bytes_to_write);
  }
  return 0;
}

size_t CombineStreamer::write_announcement(uint8_t *buffer, size_t length) {
  size_t free_bytes = this->announcement_free();
  size_t bytes_to_write = std::min(length, free_bytes);

  if (bytes_to_write > 0) {
    return this->announcement_ring_buffer_->write((void *) buffer, bytes_to_write);
  }
  return 0;
}

void CombineStreamer::start(const std::string &task_name, UBaseType_t priority) {
  if (this->task_handle_ == nullptr) {
    xTaskCreate(CombineStreamer::combine_task_, task_name.c_str(), 4096, (void *) this, priority, &this->task_handle_);
  }
}

void CombineStreamer::reset_ring_buffers() {
  this->output_ring_buffer_->reset();
  this->media_ring_buffer_->reset();
  this->announcement_ring_buffer_->reset();
}

void CombineStreamer::combine_task_(void *params) {
  CombineStreamer *this_combiner = (CombineStreamer *) params;

  TaskEvent event;
  CommandEvent command_event;

  //  ?big? assumption here is that incoming stream is 16 bits per sample... TODO: Check and verify this
  // TODO: doesn't handle different sample rates
  // TODO: doesn't handle different amount of channels
  ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  int16_t *media_buffer = allocator.allocate(BUFFER_SIZE);
  int16_t *announcement_buffer = allocator.allocate(BUFFER_SIZE);
  int16_t *combination_buffer = allocator.allocate(BUFFER_SIZE);

  if ((media_buffer == nullptr) || (announcement_buffer == nullptr)) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

  int16_t q15_ducking_ratio = (int16_t) (1 * std::pow(2, 15));  // esp-dsp using q15 fixed point numbers
  bool transfer_media = true;

  while (true) {
    if (xQueueReceive(this_combiner->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::STOP) {
        break;
      } else if (command_event.command == CommandEventType::DUCK) {
        float ducking_ratio = command_event.ducking_ratio;
        q15_ducking_ratio = (int16_t) (ducking_ratio * std::pow(2, 15));  // convert float to q15 fixed point
      } else if (command_event.command == CommandEventType::PAUSE_MEDIA) {
        transfer_media = false;
      } else if (command_event.command == CommandEventType::RESUME_MEDIA) {
        transfer_media = true;
      }
    }

    size_t media_available = this_combiner->media_ring_buffer_->available();
    size_t announcement_available = this_combiner->announcement_ring_buffer_->available();
    size_t output_free = this_combiner->output_ring_buffer_->free();

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
          media_bytes_read = this_combiner->media_ring_buffer_->read((void *) media_buffer, bytes_to_read, 0);
          if (media_bytes_read > 0) {
            if (q15_ducking_ratio < (1 * std::pow(2, 15))) {
              dsps_mulc_s16_ae32(media_buffer, combination_buffer, media_bytes_read, q15_ducking_ratio, 1, 1);
              std::memcpy((void *) media_buffer, (void *) combination_buffer, media_bytes_read);
            }
          }
        }

        size_t announcement_bytes_read = 0;
        if (announcement_available > 0) {
          announcement_bytes_read =
              this_combiner->announcement_ring_buffer_->read((void *) announcement_buffer, bytes_to_read, 0);
        }

        size_t bytes_written = 0;
        if ((media_bytes_read > 0) && (announcement_bytes_read > 0)) {
          // This adds the two signals together and then shifts it by 1 bit to avoid clipping
          // TODO: Don't shift by 1 as the announcement stream will be quieter than desired (need to clamp?)
          dsps_add_s16_aes3(media_buffer, announcement_buffer, combination_buffer, bytes_to_read, 1, 1, 1, 1);
          bytes_written = this_combiner->output_ring_buffer_->write((void *) combination_buffer, bytes_to_read);
        } else if (media_bytes_read > 0) {
          bytes_written = this_combiner->output_ring_buffer_->write((void *) media_buffer, media_bytes_read);

        } else if (announcement_bytes_read > 0) {
          bytes_written =
              this_combiner->output_ring_buffer_->write((void *) announcement_buffer, announcement_bytes_read);
        }

        if (bytes_written) {
          event.type = EventType::RUNNING;
          xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);
        } else if (this_combiner->output_ring_buffer_->available() == 0) {
          event.type = EventType::IDLE;
          xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);
        }
      }
    }
  }

  event.type = EventType::STOPPING;
  xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

  this_combiner->reset_ring_buffers();
  allocator.deallocate(media_buffer, BUFFER_SIZE);
  allocator.deallocate(announcement_buffer, BUFFER_SIZE);
  allocator.deallocate(combination_buffer, BUFFER_SIZE);

  event.type = EventType::STOPPED;
  xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

}  // namespace nabu
}  // namespace esphome
#endif
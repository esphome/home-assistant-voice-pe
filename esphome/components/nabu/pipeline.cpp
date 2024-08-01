#ifdef USE_ESP_IDF

#include "esphome/core/helpers.h"
#include "pipeline.h"

namespace esphome {
namespace nabu {

static const size_t BUFFER_SIZE = 32768 * sizeof(int16_t);  // Audio samples

static const size_t QUEUE_COUNT = 10;

Pipeline::Pipeline(CombineStreamer *mixer, PipelineType pipeline_type) {
  this->reader_ = make_unique<HTTPStreamer>();
  this->decoder_ = make_unique<DecodeStreamer>();
  this->resampler_ = make_unique<ResampleStreamer>();

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));

  this->mixer_ = mixer;
  this->pipeline_type_ = pipeline_type;
}

size_t Pipeline::read(uint8_t *buffer, size_t length) {
  size_t available_bytes = this->available();
  size_t bytes_to_read = std::min(length, available_bytes);
  if (bytes_to_read > 0) {
    return this->decoder_->read(buffer, bytes_to_read);
  }
  return 0;
}

void Pipeline::start(const std::string &uri, const std::string &task_name, UBaseType_t priority) {
  this->reader_->start(uri, task_name + "_reader");
  this->decoder_->start(task_name + "_decoder");
  this->resampler_->start(task_name + "_resampler");
  if (this->task_handle_ == nullptr) {
    xTaskCreate(Pipeline::transfer_task_, task_name.c_str(), 8096, (void *) this, priority, &this->task_handle_);
  }
}

void Pipeline::start(media_player::MediaFile *media_file, const std::string &task_name, UBaseType_t priority) {
  this->reader_->start(media_file, task_name + "_reader");
  this->decoder_->start(task_name + "_decoder");
  this->resampler_->start(task_name + "_resampler");
  if (this->task_handle_ == nullptr) {
    xTaskCreate(Pipeline::transfer_task_, task_name.c_str(), 8096, (void *) this, priority, &this->task_handle_);
  }
}
void Pipeline::stop() {
  vTaskDelete(this->task_handle_);
  this->task_handle_ = nullptr;

  xQueueReset(this->event_queue_);
  xQueueReset(this->command_queue_);
}

BaseType_t Pipeline::send_command(CommandEvent *command, TickType_t ticks_to_wait) {
  return xQueueSend(this->command_queue_, command, ticks_to_wait);
}

BaseType_t Pipeline::read_event(TaskEvent *event, TickType_t ticks_to_wait) {
  return xQueueReceive(this->event_queue_, event, ticks_to_wait);
}

void Pipeline::transfer_task_(void *params) {
  Pipeline *this_pipeline = (Pipeline *) params;

  TaskEvent event;
  CommandEvent command_event;

  event.type = EventType::STARTING;
  event.err = ESP_OK;
  xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  uint8_t *transfer_buffer = allocator.allocate(BUFFER_SIZE);
  if (transfer_buffer == nullptr) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

  bool stopping_gracefully = true;

  this_pipeline->reading_ = true;
  this_pipeline->decoding_ = true;
  this_pipeline->resampling_ = true;

  while (true) {
    if (xQueueReceive(this_pipeline->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::START) {
        this_pipeline->reader_->send_command(&command_event);
      } else if (command_event.command == CommandEventType::STOP) {
        this_pipeline->reader_->send_command(&command_event);
        this_pipeline->decoder_->send_command(&command_event);
        this_pipeline->resampler_->send_command(&command_event);
        stopping_gracefully = false;
      } else if (command_event.command == CommandEventType::STOP_GRACEFULLY) {
        this_pipeline->reader_->send_command(&command_event);
        stopping_gracefully = true;
      }
    }

    size_t bytes_to_read = 0;
    size_t bytes_read = 0;
    size_t bytes_written = 0;

    // Move data from resampler to the mixer
    if (this_pipeline->pipeline_type_ == PipelineType::MEDIA) {
      bytes_to_read = std::min(this_pipeline->mixer_->media_free(), BUFFER_SIZE);
      bytes_read = this_pipeline->resampler_->read(transfer_buffer, bytes_to_read);
      bytes_written += this_pipeline->mixer_->write_media(transfer_buffer, bytes_read);
    } else if (this_pipeline->pipeline_type_ == PipelineType::ANNOUNCEMENT) {
      bytes_to_read = std::min(this_pipeline->mixer_->announcement_free(), BUFFER_SIZE);
      bytes_read = this_pipeline->resampler_->read(transfer_buffer, bytes_to_read);
      bytes_written += this_pipeline->mixer_->write_announcement(transfer_buffer, bytes_read);
    }

    // Move data from decoder to resampler
    bytes_to_read = std::min(this_pipeline->resampler_->input_free(), BUFFER_SIZE);
    bytes_read = this_pipeline->decoder_->read(transfer_buffer, bytes_to_read);
    bytes_written = this_pipeline->resampler_->write(transfer_buffer, bytes_read);

    // Move data from http reader to decoder
    bytes_to_read = std::min(this_pipeline->decoder_->input_free(), BUFFER_SIZE);
    bytes_read = this_pipeline->reader_->read(transfer_buffer, bytes_to_read);
    bytes_written = this_pipeline->decoder_->write(transfer_buffer, bytes_read);

    this_pipeline->watch_(stopping_gracefully);

    if (!this_pipeline->reading_ && !this_pipeline->decoding_ && !this_pipeline->resampling_) {
      break;
    }
  }

  event.type = EventType::STOPPING;
  xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

  allocator.deallocate(transfer_buffer, BUFFER_SIZE);

  event.type = EventType::STOPPED;
  xQueueSend(this_pipeline->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void Pipeline::watch_(bool stopping_gracefully) {
  TaskEvent event;
  CommandEvent command_event;

  while (this->reader_->read_event(&event)) {
    switch (event.type) {
      case EventType::STARTING:
        this->reading_ = true;
        break;
      case EventType::STARTED:
        this->reading_ = true;
        command_event.command = CommandEventType::START;
        command_event.media_file_type = event.media_file_type;
        this->decoder_->send_command(&command_event);
        break;
      case EventType::IDLE:
        this->reading_ = true;
        break;
      case EventType::RUNNING:
        this->reading_ = true;
        break;
      case EventType::STOPPING:
        this->reading_ = true;
        break;
      case EventType::STOPPED: {
        if (stopping_gracefully) {
          command_event.command = CommandEventType::STOP_GRACEFULLY;
          this->decoder_->send_command(&command_event);
        }
        this->reader_->stop();
        this->reading_ = false;
        break;
      }
      case EventType::WARNING:
        this->reading_ = false;
        xQueueSend(this->event_queue_, &event, portMAX_DELAY);
        break;
    }
  }

  while (this->decoder_->read_event(&event)) {
    switch (event.type) {
      case EventType::STARTING:
        this->decoding_ = true;
        break;
      case EventType::STARTED:
        this->decoding_ = true;
        command_event.command = CommandEventType::START;
        command_event.media_file_type = event.media_file_type;
        command_event.stream_info = event.stream_info;
        this->resampler_->send_command(&command_event);
        break;
      case EventType::IDLE:
        this->decoding_ = true;
        break;
      case EventType::RUNNING:
        this->decoding_ = true;
        break;
      case EventType::STOPPING:
        this->decoding_ = true;
        break;
      case EventType::STOPPED:
        if (stopping_gracefully) {
          command_event.command = CommandEventType::STOP_GRACEFULLY;
          this->resampler_->send_command(&command_event);
        }
        this->decoder_->stop();
        this->decoding_ = false;
        break;
      case EventType::WARNING:
        this->decoding_ = false;
        xQueueSend(this->event_queue_, &event, portMAX_DELAY);
        break;
    }
  }

  while (this->resampler_->read_event(&event)) {
    switch (event.type) {
      case EventType::STARTING:
        this->resampling_ = true;
        break;
      case EventType::STARTED:
        this->resampling_ = true;
        break;
      case EventType::IDLE:
        this->resampling_ = true;
        break;
      case EventType::RUNNING:
        this->resampling_ = true;
        break;
      case EventType::STOPPING:
        this->resampling_ = true;
        break;
      case EventType::STOPPED:
        if (!stopping_gracefully) {
          if (this->pipeline_type_ == PipelineType::ANNOUNCEMENT) {
            command_event.command = CommandEventType::CLEAR_ANNOUNCEMENT;
            this->mixer_->send_command(&command_event);
          } else if (this->pipeline_type_ == PipelineType::MEDIA) {
            command_event.command = CommandEventType::CLEAR_MEDIA;
            this->mixer_->send_command(&command_event);
          }
        }
        this->resampler_->stop();
        this->resampling_ = false;
        break;
      case EventType::WARNING:
        this->resampling_ = false;
        xQueueSend(this->event_queue_, &event, portMAX_DELAY);
        break;
    }
  }
  if (this->reading_ || this->decoding_ || this->resampling_) {
    event.type = EventType::RUNNING;
    xQueueSend(this->event_queue_, &event, portMAX_DELAY);
  } else {
    event.type = EventType::IDLE;
    xQueueSend(this->event_queue_, &event, portMAX_DELAY);
  }
}

}  // namespace nabu
}  // namespace esphome
#endif
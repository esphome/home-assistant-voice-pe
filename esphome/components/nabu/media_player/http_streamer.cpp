#ifdef USE_ESP_IDF

#include "http_streamer.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
namespace esphome {
namespace nabu {

static const size_t HTTP_BUFFER_SIZE = 8192;
static const size_t BUFFER_SIZE = 2048;

static const size_t QUEUE_COUNT = 10;

HTTPStreamer::HTTPStreamer() {
  this->output_ring_buffer_ = RingBuffer::create(HTTP_BUFFER_SIZE * sizeof(int16_t));
  if (this->output_ring_buffer_ == nullptr) {
    // ESP_LOGW(TAG, "Could not allocate ring buffer");
    // this->mark_failed();
    return;
  }

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));
}

void HTTPStreamer::set_stream_uri_(esp_http_client_handle_t *client, const std::string &new_uri) {
  this->cleanup_(client);

  esp_http_client_config_t config = {
      .url = new_uri.c_str(),
      .cert_pem = nullptr,
      .disable_auto_redirect = false,
      .max_redirection_count = 10,
  };
  *client = esp_http_client_init(&config);

  if (client == nullptr) {
    // ESP_LOGE(TAG, "Failed to initialize HTTP connection");
    return;
  }

  esp_err_t err;
  if ((err = esp_http_client_open(*client, 0)) != ESP_OK) {
    // ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    this->cleanup_(client);
    return;
  }

  int content_length = esp_http_client_fetch_headers(*client);
  //   ESP_LOGD(TAG, "content_length = %d", content_length);
  if (content_length <= 0) {
    // ESP_LOGE(TAG, "Failed to get content length: %s", esp_err_to_name(err));
    this->cleanup_(client);
    return;
  }

  return;
}

void HTTPStreamer::cleanup_(esp_http_client_handle_t *client) {
  if (*client != nullptr) {
    esp_http_client_close(*client);
    esp_http_client_cleanup(*client);
    *client = nullptr;
  }
}

void HTTPStreamer::read_task_(void *params) {
  HTTPStreamer *this_streamer = (HTTPStreamer *) params;

  TaskEvent event;
  CommandEvent command_event;

  esp_http_client_handle_t client = nullptr;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  uint8_t *buffer = allocator.allocate(HTTP_BUFFER_SIZE * sizeof(int16_t));

  if (buffer == nullptr) {
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

  while (true) {
    if (xQueueReceive(this_streamer->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::START) {
        if (client == nullptr) {
          this_streamer->set_stream_uri_(&client, this_streamer->current_uri_);
        }
      } else if (command_event.command == CommandEventType::STOP) {
        this_streamer->current_uri_ = std::string();
        this_streamer->cleanup_(&client);
        break;
      }
    }

    if ((client != nullptr)) {
      size_t read_bytes = this_streamer->output_ring_buffer_->free();
      int received_len = esp_http_client_read(client, (char *) buffer, read_bytes);

      if (received_len > 0) {
        size_t bytes_written = this_streamer->output_ring_buffer_->write((void *) buffer, received_len);
      } else if (received_len < 0) {
        // Error situation
      }

      if (esp_http_client_is_complete_data_received(client)) {
        this_streamer->current_uri_ = std::string();
        this_streamer->cleanup_(&client);
      }

      event.type = EventType::RUNNING;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else if (this_streamer->output_ring_buffer_->available() > 0) {
      // the connection is closed but there is still data in the ring buffer
      event.type = EventType::IDLE;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else {
      // there is no active connection and the ring buffer is empty, so move to end task
      break;
    }
  }
  event.type = EventType::STOPPING;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  allocator.deallocate(buffer, HTTP_BUFFER_SIZE);

  event.type = EventType::STOPPED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

CombineStreamer::CombineStreamer() {
  this->output_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  this->media_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  this->announcement_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  if ((this->output_ring_buffer_ == nullptr) || (this->media_ring_buffer_ == nullptr) ||
      ((this->output_ring_buffer_ == nullptr))) {
    // ESP_LOGW(TAG, "Could not allocate ring buffer");
    // this->mark_failed();
    return;
  }

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));
}

void CombineStreamer::combine_task_(void *params) {
  CombineStreamer *this_combiner = (CombineStreamer *) params;

  TaskEvent event;
  CommandEvent command_event;

  esp_http_client_handle_t client = nullptr;

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
  uint8_t duck_bits = 0;
  while (true) {
    if (xQueueReceive(this_combiner->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::STOP) {
        break;
      } else if (command_event.command == CommandEventType::DUCK) {
        duck_bits = command_event.duck_bits;
      }
    }

    size_t media_available = this_combiner->media_ring_buffer_->available();
    size_t announcement_available = this_combiner->announcement_ring_buffer_->available();
    size_t output_free = this_combiner->output_ring_buffer_->free();

    if (output_free > 0) {
      size_t bytes_to_read = output_free;

      if (media_available > 0) {
        bytes_to_read = std::min(bytes_to_read, media_available);
      }

      if (announcement_available > 0) {
        bytes_to_read = std::min(bytes_to_read, announcement_available);
      }

      size_t media_bytes_read = 0;
      if (media_available > 0) {
        media_bytes_read = this_combiner->media_ring_buffer_->read((void *) media_buffer, bytes_to_read, 0);
        if (media_bytes_read > 0) {
          if (duck_bits > 0) {
            for (int i = 0; i < media_bytes_read / sizeof(int16_t); ++i) {
              media_buffer[i] = media_buffer[i] >> duck_bits;
            }
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
        for (int i = 0; i < bytes_to_read / sizeof(int16_t); ++i) {
          int32_t combined_sample = (static_cast<int32_t>(media_buffer[i]) )+ (static_cast<int32_t>(announcement_buffer[i]));
          combination_buffer[i] = clamp<int16_t>(combined_sample, INT16_MIN, INT16_MAX);
        }
        bytes_written = this_combiner->output_ring_buffer_->write((void *) combination_buffer, bytes_to_read);
      } else if (media_bytes_read > 0) {
        bytes_written = this_combiner->output_ring_buffer_->write((void *) media_buffer, bytes_to_read);
      } else {
        bytes_written = this_combiner->output_ring_buffer_->write((void *) announcement_buffer, bytes_to_read);
      }
    }

    if (this_combiner->output_ring_buffer_->available() == 0) {
      // the connection is closed but there is still data in the ring buffer
      event.type = EventType::IDLE;
      xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);
    }
  }
  event.type = EventType::STOPPING;
  xQueueSend(this_combiner->event_queue_, &event, portMAX_DELAY);

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
#ifdef USE_ESP_IDF

#include "streamer.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include <esp_http_client.h>

namespace esphome {
namespace nabu {

// Major TODOs:
//  - Rename/split up file, it contains more than one class

static const size_t HTTP_BUFFER_SIZE = 16 * 8192;
static const size_t QUEUE_COUNT = 20;


void OutputStreamer::stop() {
  vTaskDelete(this->task_handle_);
  this->task_handle_ = nullptr;

  xQueueReset(this->event_queue_);
  xQueueReset(this->command_queue_);
}


HTTPStreamer::HTTPStreamer() {
  this->output_ring_buffer_ = RingBuffer::create(HTTP_BUFFER_SIZE);
  // TODO: Handle if this fails to allocate
  if (this->output_ring_buffer_ == nullptr) {
    return;
  }

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));
}

MediaFileType HTTPStreamer::establish_connection_(esp_http_client_handle_t *client) {
  this->cleanup_connection_(client);

  if (this->current_uri_.empty()) {
    return MediaFileType::NONE;
  }

  esp_http_client_config_t config = {
      .url = this->current_uri_.c_str(),
      .cert_pem = nullptr,
      .disable_auto_redirect = false,
      .max_redirection_count = 10,
  };
  *client = esp_http_client_init(&config);

  if (client == nullptr) {
    printf("Failed to initialize HTTP connection");
    return MediaFileType::NONE;
  }

  esp_err_t err;
  if ((err = esp_http_client_open(*client, 0)) != ESP_OK) {
    printf("Failed to open HTTP connection");
    this->cleanup_connection_(client);
    return MediaFileType::NONE;
  }

  int content_length = esp_http_client_fetch_headers(*client);

  // TODO: Figure out how to handle this better! Music Assistant streams don't send a content length
  // if (content_length <= 0) {
  //   printf("Fialed to get content length");
  //   this->cleanup_connection_(client);
  //   return MediaFileType::NONE;
  // }

  char url[500];
  if (esp_http_client_get_url(*client, url, 500) != ESP_OK) {
    this->cleanup_connection_(client);
    return MediaFileType::NONE;
  }

  std::string url_string = url;

  if (str_endswith(url_string, ".wav")) {
    return MediaFileType::WAV;
  } else if (str_endswith(url_string, ".mp3")) {
    return MediaFileType::MP3;
  } else if (str_endswith(url_string, ".flac")) {
    return MediaFileType::FLAC;
  }

  return MediaFileType::NONE;
}

void HTTPStreamer::start(const std::string &task_name, UBaseType_t priority) {
  if (this->task_handle_ == nullptr) {
    xTaskCreate(HTTPStreamer::read_task_, task_name.c_str(), 3072, (void *) this, priority, &this->task_handle_);
  }
}

void HTTPStreamer::start(const std::string &uri, const std::string &task_name, UBaseType_t priority) {
  this->current_uri_ = uri;
  this->start(task_name, priority);
  CommandEvent command_event;
  command_event.command = CommandEventType::START;
  this->send_command(&command_event);
}

void HTTPStreamer::cleanup_connection_(esp_http_client_handle_t *client) {
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
  uint8_t *buffer = allocator.allocate(HTTP_BUFFER_SIZE);

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

  MediaFileType file_type = MediaFileType::NONE;

  while (true) {
    if (xQueueReceive(this_streamer->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::START) {
        file_type = this_streamer->establish_connection_(&client);
        if (file_type == MediaFileType::NONE) {
          this_streamer->cleanup_connection_(&client);
          break;
        } else {
          event.type = EventType::STARTED;
          event.media_file_type = file_type;
          xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
        }
      } else if (command_event.command == CommandEventType::STOP) {
        this_streamer->cleanup_connection_(&client);
        break;
      } else if (command_event.command == CommandEventType::STOP_GRACEFULLY) {
        // Waits until output ring buffer is empty before stopping the loop
        this_streamer->cleanup_connection_(&client);
      }
    }

    if (client != nullptr) {
      size_t bytes_to_read = this_streamer->output_ring_buffer_->free();
      int received_len = 0;
      if (bytes_to_read > 0) {
        received_len = esp_http_client_read(client, (char *) buffer, bytes_to_read);
      }

      if (received_len > 0) {
        size_t bytes_written = this_streamer->output_ring_buffer_->write((void *) buffer, received_len);
      } else if (received_len < 0) {
        // Error situation
      }

      if (esp_http_client_is_complete_data_received(client)) {
        this_streamer->cleanup_connection_(&client);
      }

      event.type = EventType::RUNNING;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else if (this_streamer->output_ring_buffer_->available() > 0) {
      // the connection is closed but there is still data in the ring buffer
      event.type = EventType::IDLE;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else if (file_type != MediaFileType::NONE) {
      // there is no active connection, the ring buffer is empty, and a file was actually read, so move to end task
      break;
    }
  }
  event.type = EventType::STOPPING;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  this_streamer->reset_ring_buffers();
  allocator.deallocate(buffer, HTTP_BUFFER_SIZE);

  event.type = EventType::STOPPED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

}  // namespace nabu
}  // namespace esphome

#endif
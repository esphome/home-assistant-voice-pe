#pragma once

#ifdef USE_ESP_IDF

#include"http_streamer.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace nabu {

static const size_t BUFFER_MS = 500;
// static const size_t BUFFER_SIZE = BUFFER_MS * (16000 / 1000) * sizeof(int16_t);
static const size_t RECEIVE_SIZE = 1024;
static const size_t SPEAKER_BUFFER_SIZE = 16 * RECEIVE_SIZE;

static const size_t QUEUE_COUNT = 10;

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

HTTPStreamer::HTTPStreamer() {
  this->output_ring_buffer_ = RingBuffer::create(SPEAKER_BUFFER_SIZE * sizeof(int16_t));
  if (this->output_ring_buffer_ == nullptr) {
    // ESP_LOGW(TAG, "Could not allocate ring buffer");
    // this->mark_failed();
    return;
  }

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(MediaCommandEvent));
}

void HTTPStreamer::cleanup_(esp_http_client_handle_t *client) {
  if (client != nullptr) {
    esp_http_client_cleanup(*client);
    client = nullptr;
  }
}

void HTTPStreamer::read_task_(void *params) {
  HTTPStreamer *this_streamer = (HTTPStreamer *) params;

  TaskEvent event;
  MediaCommandEvent media_command_event;

  esp_http_client_handle_t client = nullptr;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  uint8_t *buffer = allocator.allocate(SPEAKER_BUFFER_SIZE * sizeof(int16_t));

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

  bool is_feeding = false;

  while (true) {
    if (xQueueReceive(this_streamer->command_queue_, &media_command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (media_command_event.command == media_player::MEDIA_PLAYER_COMMAND_PLAY) {
        if (client == nullptr) {
          // no client has been established
          this_streamer->set_stream_uri_(&client, this_streamer->current_uri_);
        } else {
          // check if we have a new url, if so, restart the client
          // FIX! Comparing URLs doesn't owrk as esp_http_client strips the authentication stuff in the original url

          // char old_url[2048];
          // esp_http_client_get_url(client, old_url, sizeof(old_url));
          // ESP_LOGD(TAG, "old client url: %s", old_url);
          // ESP_LOGD(TAG, "url saved in component: %s", this_streamer->current_uri_.c_str());
          // ESP_LOGD(TAG, "string compare =%d", strcmp(old_url, this_streamer->current_uri_.c_str()));
          // if (!strcmp(old_url, this_streamer->current_uri_.c_str())) {
          //   this_streamer->set_stream_uri_(&client, this_streamer->current_uri_);
          // }
        }
        is_feeding = true;
      } else if (media_command_event.command == media_player::MEDIA_PLAYER_COMMAND_PAUSE) {
        is_feeding = false;
      } else if (media_command_event.command == media_player::MEDIA_PLAYER_COMMAND_STOP) {
        is_feeding = false;
        this_streamer->current_uri_ = std::string();
        this_streamer->cleanup_(&client);
        break;
      }
    }

    if ((client != nullptr) && is_feeding) {
      if (esp_http_client_is_complete_data_received(client)) {
        is_feeding = false;
        this_streamer->current_uri_ = std::string();
        this_streamer->cleanup_(&client);
        break;
      }
      size_t read_bytes = this_streamer->output_ring_buffer_->free();
      int received_len = esp_http_client_read(client, (char *) buffer, read_bytes);

      if (received_len > 0) {
        size_t bytes_written = this_streamer->output_ring_buffer_->write((void *) buffer, received_len);
      } else if (received_len < 0) {
        // Error situation
      }

      event.type = EventType::RUNNING;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else {
      event.type = EventType::IDLE;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    }
  }

  event.type = EventType::STOPPED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}
}  // namespace nabu
}  // namespace esphome

#endif
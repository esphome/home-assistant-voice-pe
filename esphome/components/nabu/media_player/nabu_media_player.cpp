#include "nabu_media_player.h"

#ifdef USE_ESP_IDF

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nabu {

static const char *const TAG = "nabu_media_player";
static const size_t BUFFER_MS = 500;
// static const size_t BUFFER_SIZE = BUFFER_MS * (16000 / 1000) * sizeof(int16_t);
static const size_t RECEIVE_SIZE = 1024;
static const size_t SPEAKER_BUFFER_SIZE = 16 * RECEIVE_SIZE;

static const size_t QUEUE_COUNT = 10;

void NabuMediaPlayer::setup() {
  state = media_player::MEDIA_PLAYER_STATE_IDLE;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->transfer_buffer_ = allocator.allocate(SPEAKER_BUFFER_SIZE * sizeof(int16_t));
  if (this->transfer_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate transfer buffer");
    this->mark_failed();
    return;
  }

  this->output_ring_buffer_ = RingBuffer::create(SPEAKER_BUFFER_SIZE * sizeof(int16_t));
  if (this->output_ring_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate ring buffer");
    this->mark_failed();
    return;
  }

  this->http_read_event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->http_read_command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(MediaCommandEvent));

  ESP_LOGI(TAG, "Set up nabu media player");
}

void NabuMediaPlayer::cleanup_(esp_http_client_handle_t *client) {
  if (client != nullptr) {
    esp_http_client_cleanup(*client);
    client = nullptr;
  }
}

void NabuMediaPlayer::http_read_task_(void *params) {
  NabuMediaPlayer *this_media_player = (NabuMediaPlayer *) params;

  TaskEvent event;
  MediaCommandEvent media_command_event;

  esp_http_client_handle_t client = nullptr;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  uint8_t *buffer = allocator.allocate(SPEAKER_BUFFER_SIZE * sizeof(int16_t));

  if (buffer == nullptr) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_media_player->http_read_event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_media_player->http_read_event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_media_player->http_read_event_queue_, &event, portMAX_DELAY);

  bool is_feeding = false;

  while (true) {
    if (xQueueReceive(this_media_player->http_read_command_queue_, &media_command_event, (10 / portTICK_PERIOD_MS)) ==
        pdTRUE) {
      if (media_command_event.command == media_player::MEDIA_PLAYER_COMMAND_PLAY) {
        if (client == nullptr) {
          // no client has been established
          this_media_player->set_stream_uri_(&client, this_media_player->current_uri_);
        } else {
          // check if we have a new url, if so, restart the client
          // FIX! Comparing URLs doesn't owrk as esp_http_client strips the authentication stuff in the original url
          
          // char old_url[2048];
          // esp_http_client_get_url(client, old_url, sizeof(old_url));
          // ESP_LOGD(TAG, "old client url: %s", old_url);
          // ESP_LOGD(TAG, "url saved in component: %s", this_media_player->current_uri_.c_str());
          // ESP_LOGD(TAG, "string compare =%d", strcmp(old_url, this_media_player->current_uri_.c_str()));
          // if (!strcmp(old_url, this_media_player->current_uri_.c_str())) {
          //   this_media_player->set_stream_uri_(&client, this_media_player->current_uri_);
          // }
        }
        is_feeding = true;
      } else if (media_command_event.command == media_player::MEDIA_PLAYER_COMMAND_PAUSE) {
        is_feeding = false;
      } else if (media_command_event.command == media_player::MEDIA_PLAYER_COMMAND_STOP) {
        is_feeding = false;
        this_media_player->current_uri_ = std::string();
        this_media_player->cleanup_(&client);
        break;
      }
    }

    if ((client != nullptr) && is_feeding) {
      if (esp_http_client_is_complete_data_received(client)) {
        is_feeding = false;
        this_media_player->current_uri_ = std::string();
        this_media_player->cleanup_(&client);
        break;
      }
      size_t read_bytes = this_media_player->output_ring_buffer_->free();
      int received_len = esp_http_client_read(client, (char *) buffer, read_bytes);

      if (received_len > 0) {
        size_t bytes_written = this_media_player->output_ring_buffer_->write((void *) buffer, received_len);
      } else if (received_len < 0) {
        // Error situation
      }

      event.type = EventType::RUNNING;
      xQueueSend(this_media_player->http_read_event_queue_, &event, portMAX_DELAY);
    } else {
      event.type = EventType::IDLE;
      xQueueSend(this_media_player->http_read_event_queue_, &event, portMAX_DELAY);
    }
  }

  event.type = EventType::STOPPED;
  xQueueSend(this_media_player->http_read_event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void NabuMediaPlayer::watch_() {
  TaskEvent event;
  while (xQueueReceive(this->http_read_event_queue_, &event, 0)) {
    switch (event.type) {
      case EventType::STARTING:
        ESP_LOGD(TAG, "Starting HTTP Media Playback");
        break;
      case EventType::STARTED:
        ESP_LOGD(TAG, "Started HTTP Media Playback");
        break;
      case EventType::IDLE:
        this->is_playing_ = false;

        if (this->current_uri_.empty()) {
          this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
        } else {
          this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
        }

        break;
      case EventType::RUNNING:
        this->is_playing_ = true;
        this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;

        this->status_clear_warning();
        break;
      case EventType::STOPPING:
        ESP_LOGD(TAG, "Stopping HTTP Media Playback");
        break;
      case EventType::STOPPED:
        this->state = media_player::MEDIA_PLAYER_STATE_IDLE;

        vTaskDelete(this->http_read_task_handle_);
        this->http_read_task_handle_ = nullptr;

        this->output_ring_buffer_->reset();
        xQueueReset(this->http_read_event_queue_);
        xQueueReset(this->http_read_command_queue_);

        ESP_LOGD(TAG, "Stopped HTTP Media Playback");
        break;
      case EventType::WARNING:
        ESP_LOGW(TAG, "Error reading HTTP media: %s", esp_err_to_name(event.err));
        this->status_set_warning();
        break;
    }
  }
  this->publish_state();
}

bool NabuMediaPlayer::set_stream_uri_(esp_http_client_handle_t *client, const std::string &new_uri) {
  this->cleanup_(client);

  esp_http_client_config_t config = {
      .url = new_uri.c_str(),
      .cert_pem = nullptr,
      .disable_auto_redirect = false,
      .max_redirection_count = 10,
  };
  *client = esp_http_client_init(&config);

  if (client == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize HTTP connection");
    return false;
  }

  esp_err_t err;
  if ((err = esp_http_client_open(*client, 0)) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    this->cleanup_(client);
    return false;
  }

  int content_length = esp_http_client_fetch_headers(*client);
  ESP_LOGD(TAG, "content_length = %d", content_length);
  if (content_length <= 0) {
    ESP_LOGE(TAG, "Failed to get content length: %s", esp_err_to_name(err));
    this->cleanup_(client);
    return false;
  }

  if (!this->read_wav_header_(client)) {
    ESP_LOGE(TAG, "Failed to read wave file header");
    this->cleanup_(client);
    return false;
  }

  return true;
}

void NabuMediaPlayer::loop() {
  this->watch_();

  if (this->is_playing_) {
    size_t speaker_free_bytes = this->speaker_->free_bytes();
    size_t internal_buffer_available_bytes = this->output_ring_buffer_->available();

    size_t bytes_to_transfer = std::min(speaker_free_bytes, internal_buffer_available_bytes);

    size_t bytes_read = this->output_ring_buffer_->read((void *) this->transfer_buffer_, bytes_to_transfer);

    if (bytes_read > 0) {
      this->speaker_->write(this->transfer_buffer_, bytes_read);
    }
  }
}

void NabuMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  MediaCommandEvent media_command_event;
  if (call.get_media_url().has_value()) {
    this->current_uri_ = call.get_media_url().value();

    if (this->http_read_task_handle_ == nullptr) {
      xTaskCreate(NabuMediaPlayer::http_read_task_, "http_read_task", 8096, (void *) this, 1,
                  &this->http_read_task_handle_);
    }
    media_command_event.command = media_player::MEDIA_PLAYER_COMMAND_PLAY;
    xQueueSend(this->http_read_command_queue_, &media_command_event, portMAX_DELAY);
    return;
  }

  // if (call.get_volume().has_value()) {
  //   set_volume_(call.get_volume().value());
  //   unmute_();
  // }

  if (call.get_command().has_value()) {
    switch (call.get_command().value()) {
      case media_player::MEDIA_PLAYER_COMMAND_PLAY:
        if (state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
          media_command_event.command = call.get_command().value();
          xQueueSend(this->http_read_command_queue_, &media_command_event, portMAX_DELAY);
        }
        break;
      case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
        media_command_event.command = call.get_command().value();
        xQueueSend(this->http_read_command_queue_, &media_command_event, portMAX_DELAY);
        break;
      case media_player::MEDIA_PLAYER_COMMAND_STOP:
        media_command_event.command = call.get_command().value();
        xQueueSend(this->http_read_command_queue_, &media_command_event, portMAX_DELAY);
        break;
      case media_player::MEDIA_PLAYER_COMMAND_MUTE:
        this->is_playing_ = false;
        break;
      case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
        this->is_playing_ = true;
        break;
      case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
        if (state == media_player::MEDIA_PLAYER_STATE_PAUSED) {
          media_command_event.command = media_player::MEDIA_PLAYER_COMMAND_PLAY;
          xQueueSend(this->http_read_command_queue_, &media_command_event, portMAX_DELAY);
        } else {
          media_command_event.command = media_player::MEDIA_PLAYER_COMMAND_PAUSE;
          xQueueSend(this->http_read_command_queue_, &media_command_event, portMAX_DELAY);
        }
        break;
        //   case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP: {
        //     //       float new_volume = this->volume + 0.1f;
        //     //       if (new_volume > 1.0f)
        //     //         new_volume = 1.0f;
        //     //       set_volume_(new_volume);
        //     //       unmute_();
        //     break;
        //   }
        //   case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN: {
        //     // float new_volume = this->volume - 0.1f;
        //     // if (new_volume < 0.0f)
        //     //   new_volume = 0.0f;
        //     // set_volume_(new_volume);
        //     // unmute_();
        //     break;
        //   }
      default:
        break;
    }
  }
}

// pausing is only supported if destroy_pipeline_on_stop is disabled
media_player::MediaPlayerTraits NabuMediaPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.set_supports_pause(true);
  return traits;
};

bool NabuMediaPlayer::read_wav_header_(esp_http_client_handle_t *client) {
  uint8_t header_control_counter = 0;
  char data[4];
  uint8_t subchunk_extra_data = 0;

  while (header_control_counter <= 10) {
    size_t bytes_read = esp_http_client_read(*client, data, 4);
    if (bytes_read != 4) {
      ESP_LOGE(TAG, "Failed to read from header file");
    }

    if (header_control_counter == 0) {
      ++header_control_counter;
      if ((data[0] != 'R') || (data[1] != 'I') || (data[2] != 'F') || (data[3] != 'F')) {
        ESP_LOGW(TAG, "file has no RIFF tag");
        return false;
      }
    } else if (header_control_counter == 1) {
      ++header_control_counter;
      uint32_t chunk_size = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));
    } else if (header_control_counter == 2) {
      ++header_control_counter;
      if ((data[0] != 'W') || (data[1] != 'A') || (data[2] != 'V') || (data[3] != 'E')) {
        ESP_LOGW(TAG, "format tag is not WAVE");
        return false;
      }
    } else if (header_control_counter == 3) {
      ++header_control_counter;
      if ((data[0] != 'f') || (data[1] != 'm') || (data[2] != 't')) {
        ESP_LOGW(TAG, "Improper wave file header");
        return false;
      }
    } else if (header_control_counter == 4) {
      ++header_control_counter;

      uint32_t chunk_size = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));

      if (chunk_size != 16) {
        ESP_LOGW(TAG, "Audio is not PCM data, can't play");
        return false;
      }
    } else if (header_control_counter == 5) {
      ++header_control_counter;

      uint32_t chunk_size = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));

      // if (chunk_size != 16) {
      //   ESP_LOGW(TAG, "Audio is not PCM data, can't play");
      //   return false;
      // }
    } else if (header_control_counter == 6) {
      ++header_control_counter;

      uint16_t fc = (uint16_t) (data[0] + (data[1] << 8));         // Format code
      uint16_t nic = (uint16_t) ((data[2] + 2) + (data[3] << 8));  // Number of interleaved channels

      // if (fc != 1) {
      //   ESP_LOGW(TAG, "Audio is not PCM data, can't play");
      //   return false;
      // }

      if (nic > 2) {
        ESP_LOGW(TAG, "Can only play mono or stereo channel audio");
        return false;
      }
      // this->stream_info_.channels = (speaker::AudioChannels) nic;
    } else if (header_control_counter == 7) {
      ++header_control_counter;

      uint32_t sample_rate = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));

      // this->stream_info_.sample_rate = sample_rate;
    } else if (header_control_counter == 8) {
      ++header_control_counter;

      uint32_t byte_rate = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));
    } else if (header_control_counter == 8) {
      ++header_control_counter;

      uint16_t block_align = (uint16_t) (data[0] + (data[1] << 8));
      uint16_t bits_per_sample = (uint16_t) ((data[2] + 2) + (data[3] << 8));

      if ((bits_per_sample != 16)) {
        ESP_LOGW(TAG, "Can only play wave flies with 16 bits per sample");
        return false;
      }
      // this->stream_info_.bits_per_sample = (speaker::AudioSamplesPerBit) bits_per_sample;
    } else if (header_control_counter == 9) {
      ++header_control_counter;
      if ((data[0] != 'd') || (data[1] != 'a') || (data[2] != 't') || (data[3] != 'a')) {
        ESP_LOGW(TAG, "Improper Wave Header");
        // return false;
      }
    } else if (header_control_counter == 10) {
      ++header_control_counter;
      uint32_t chunk_size = (uint32_t) (data[0] + (data[1] << 8) + (data[2] << 16) + (data[3] << 24));
    } else {
      ESP_LOGW(TAG, "Unknown state when reading wave file header");
      // return false;
    }
  }

  return true;
}

}  // namespace nabu
}  // namespace esphome
#endif

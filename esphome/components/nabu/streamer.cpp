#ifdef USE_ESP_IDF

#include "streamer.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

#include "esp_dsp.h"

#include "mp3_decoder.h"
// #include "flac_decoder.h"

namespace esphome {
namespace nabu {

// Major TODOs:
//  - Rename/split up file, it contains more than one class

static const size_t HTTP_BUFFER_SIZE = 2 * 8192;
static const size_t BUFFER_SIZE = 2048;

static const size_t QUEUE_COUNT = 10;

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
    xTaskCreate(ResampleStreamer::resample_task_, task_name.c_str(), 4092, (void *) this, priority,
                &this->task_handle_);
  }
}

// void ResampleStreamer::stop() {
//   vTaskDelete(this->task_handle_);
//   this->task_handle_ = nullptr;

//   xQueueReset(this->event_queue_);
//   xQueueReset(this->command_queue_);
// }

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
  int16_t *input_buffer = allocator.allocate(BUFFER_SIZE);   // * sizeof(int16_t));
  int16_t *output_buffer = allocator.allocate(BUFFER_SIZE);  // * sizeof(int16_t));

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

  bool stopping = false;

  while (true) {
    if (xQueueReceive(this_streamer->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::START) {
        // Need to receive stream information here

        media_file_type = command_event.media_file_type;

        input_buffer_current = input_buffer;
        output_buffer_current = output_buffer;
        input_buffer_length = 0;
        output_buffer_length = 0;  // measured in bytes
      } else if (command_event.command == CommandEventType::STOP) {
        break;
      } else if (command_event.command == CommandEventType::STOP_GRACEFULLY) {
        stopping = true;
      }
    }

    // if (media_file_type == MediaFileType::NONE) {
    //   continue;
    // }

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
        size_t bytes_read = this_streamer->input_ring_buffer_->read((void *) new_input_buffer_data, bytes_to_read);

        input_buffer_length += bytes_read;
      }

      /////
      // Resample here
      /////

      size_t bytes_to_transfer =
          std::min(BUFFER_SIZE * sizeof(int16_t) / 2,
                   input_buffer_length);  // Divide by two since we will convert mono to stereo in place
      std::memcpy((void *) output_buffer, (void *) input_buffer_current, bytes_to_transfer);

      input_buffer_current += bytes_to_transfer / sizeof(int16_t);
      input_buffer_length -= bytes_to_transfer;

      output_buffer_current = output_buffer;
      output_buffer_length += bytes_to_transfer;

      //////
      // Convert mono to stereo
      //////

      for (int i = output_buffer_length / (sizeof(int16_t)) - 1; i >= 0; --i) {
        output_buffer[2 * i] = output_buffer[i];
        output_buffer[2 * i + 1] = output_buffer[i];
      }

      output_buffer_length *= 2; // double the bytes for stereo samples
    }

    if (this_streamer->input_ring_buffer_->available() || this_streamer->output_ring_buffer_->available()) {
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
  allocator.deallocate(input_buffer, BUFFER_SIZE);   // * sizeof(int16_t));
  allocator.deallocate(output_buffer, BUFFER_SIZE);  // * sizeof(int16_t));

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

DecodeStreamer::DecodeStreamer() {
  this->input_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));
  this->output_ring_buffer_ = RingBuffer::create(BUFFER_SIZE * sizeof(int16_t));

  this->event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));
  this->command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));

  // TODO: Handle if this fails to allocate
  if ((this->input_ring_buffer_) || (this->output_ring_buffer_ == nullptr)) {
    return;
  }
}

void DecodeStreamer::start(const std::string &task_name, UBaseType_t priority) {
  if (this->task_handle_ == nullptr) {
    xTaskCreate(DecodeStreamer::decode_task_, task_name.c_str(), 4092, (void *) this, priority, &this->task_handle_);
  }
}

void OutputStreamer::stop() {
  vTaskDelete(this->task_handle_);
  this->task_handle_ = nullptr;

  xQueueReset(this->event_queue_);
  xQueueReset(this->command_queue_);
}

size_t DecodeStreamer::write(uint8_t *buffer, size_t length) {
  size_t free_bytes = this->input_ring_buffer_->free();
  size_t bytes_to_write = std::min(length, free_bytes);
  if (bytes_to_write > 0) {
    return this->input_ring_buffer_->write((void *) buffer, bytes_to_write);
  }
  return 0;
}

void DecodeStreamer::decode_task_(void *params) {
  DecodeStreamer *this_streamer = (DecodeStreamer *) params;

  TaskEvent event;
  CommandEvent command_event;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  uint8_t *buffer = allocator.allocate(BUFFER_SIZE);         // * sizeof(int16_t));
  uint8_t *buffer_output = allocator.allocate(BUFFER_SIZE);  // * sizeof(int16_t));

  if ((buffer == nullptr) || (buffer_output == nullptr)) {
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

  // TODO: only initialize if needed
  HMP3Decoder mp3_decoder = MP3InitDecoder();
  MP3FrameInfo mp3_frame_info;
  int mp3_bytes_left = 0;

  uint8_t *mp3_buffer_current = buffer;
  bool mp3_printed_info = false;

  int mp3_output_bytes_left = 0;
  uint8_t *mp3_output_buffer_current = buffer_output;

  bool stopping = false;
  bool header_parsed = false;
  while (true) {
    if (xQueueReceive(this_streamer->command_queue_, &command_event, (10 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::START) {
        if ((media_file_type == MediaFileType::NONE) || (media_file_type == MediaFileType::MP3)) {
          MP3FreeDecoder(mp3_decoder);
        }

        // Reset state of everything
        this_streamer->reset_ring_buffers();
        memset((void *) buffer, 0, BUFFER_SIZE);
        memset((void *) buffer_output, 0, BUFFER_SIZE);

        mp3_bytes_left = 0;

        mp3_buffer_current = buffer;
        mp3_printed_info = false;

        mp3_output_bytes_left = 0;
        mp3_output_buffer_current = buffer_output;

        stopping = false;
        header_parsed = false;

        media_file_type = command_event.media_file_type;
        if (media_file_type == MediaFileType::MP3) {
          mp3_decoder = MP3InitDecoder();
        }
      } else if (command_event.command == CommandEventType::STOP) {
        break;
      } else if (command_event.command == CommandEventType::STOP_GRACEFULLY) {
        stopping = true;
      }
    }

    if (media_file_type == MediaFileType::NONE) {
      continue;
    }

    size_t bytes_available = this_streamer->input_ring_buffer_->available();
    // we will need to know how much we can fit in the output buffer as well depending the file type
    size_t bytes_free = this_streamer->output_ring_buffer_->free();

    size_t max_bytes_to_read = std::min(bytes_free, bytes_available);
    size_t bytes_read = 0;

    // TODO: Pass on the streaming audio configuration to the mixer after determining from header; e.g., mono vs stereo,
    // sample rate, bits per sample
    if (media_file_type == MediaFileType::WAV) {
      if (!header_parsed) {
        header_parsed = true;
        bytes_read = this_streamer->input_ring_buffer_->read((void *) buffer, 44);
        max_bytes_to_read -= bytes_read;
        // TODO: Actually parse the header!
        // TODO: Pass on the stream information to mixer
      }
      size_t bytes_written = 0;
      size_t bytes_read = 0;
      size_t bytes_to_read = std::min(max_bytes_to_read, BUFFER_SIZE);
      if (max_bytes_to_read > 0) {
        bytes_read = this_streamer->input_ring_buffer_->read((void *) buffer, bytes_to_read);
      }

      if (bytes_read > 0) {
        bytes_written = this_streamer->output_ring_buffer_->write((void *) buffer, bytes_read);
      }
    } else if (media_file_type == MediaFileType::MP3) {
      if (mp3_output_bytes_left > 0) {
        size_t bytes_free = this_streamer->output_ring_buffer_->free();

        size_t bytes_to_write = std::min(static_cast<size_t>(mp3_output_bytes_left), bytes_free);

        size_t bytes_written = 0;
        if (bytes_to_write > 0) {
          bytes_written = this_streamer->output_ring_buffer_->write((void *) mp3_output_buffer_current, bytes_to_write);
        }

        mp3_output_bytes_left -= bytes_written;
        mp3_output_buffer_current += bytes_written;

      } else {
        // Shift unread data in buffer to start
        if ((mp3_bytes_left > 0) && (mp3_bytes_left < BUFFER_SIZE)) {
          memmove(buffer, mp3_buffer_current, mp3_bytes_left);
        }
        mp3_buffer_current = buffer;

        // read in new mp3 data to fill the buffer
        size_t bytes_available = this_streamer->input_ring_buffer_->available();
        size_t bytes_to_read = std::min(bytes_available, BUFFER_SIZE - mp3_bytes_left);
        if (bytes_to_read > 0) {
          uint8_t *new_mp3_data = buffer + mp3_bytes_left;
          bytes_read = this_streamer->input_ring_buffer_->read((void *) new_mp3_data, bytes_to_read);

          if (bytes_read < (BUFFER_SIZE - mp3_bytes_left)) {
            // set the remaining part of the buffer to 0 if it wasn't completely filled
            memset((void *) (buffer + mp3_bytes_left + bytes_read), 0, BUFFER_SIZE - mp3_bytes_left - bytes_read);
          }

          // update pointers
          mp3_bytes_left += bytes_read;
        }

        if (mp3_bytes_left > 0) {
          // Look for the next sync word
          int32_t offset = MP3FindSyncWord(mp3_buffer_current, mp3_bytes_left);
          if (offset < 0) {
            event.type = EventType::WARNING;
            event.err = ESP_ERR_NO_MEM;
            xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
            continue;
          }

          // Advance read pointer
          mp3_buffer_current += offset;
          mp3_bytes_left -= offset;

          int err = MP3Decode(mp3_decoder, &mp3_buffer_current, &mp3_bytes_left, (int16_t *) buffer_output, 0);
          if (err) {
            switch (err) {
              case ERR_MP3_MAINDATA_UNDERFLOW:
                // Not a problem. Next call to decode will provide more data.
                continue;
                break;
              case ERR_MP3_INDATA_UNDERFLOW:
                // event.type = EventType::WARNING;
                // event.err = ESP_ERR_INVALID_MAC;
                // xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
                break;
              default:
                // TODO: Better handle mp3 decoder errors
                // Not much we can do
                // ESP_LOGD("mp3_decoder", "Unexpected error decoding MP3 data: %d.", err);
                // event.type = EventType::WARNING;
                // event.err = ESP_ERR_INVALID_ARG;
                // xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
                break;
            }
          } else {
            // Actual audio...maybe. The frame info struct:
            // int bitrate;
            // int nChans;
            // int samprate;
            // int bitsPerSample;
            // int outputSamps;
            // int layer;
            // int version;

            // int bytes_decoded = (mp3_bytes_left - old_bytes_left);
            // mp3_buffer_current += bytes_decoded;

            MP3GetLastFrameInfo(mp3_decoder, &mp3_frame_info);
            if (mp3_frame_info.outputSamps > 0) {
              int bytes_per_sample = (mp3_frame_info.bitsPerSample / 8) * mp3_frame_info.nChans;
              mp3_output_bytes_left = mp3_frame_info.outputSamps * bytes_per_sample;
              mp3_output_buffer_current = buffer_output;
              // Only bother if there are actual output samples
              // if (!mp3_printed_info) {
              // For debugging purposes.
              // TODO: Resample
              // mp3_printed_info = true;
              // ESP_LOGD(TAG, "Sample Rate: %d", mp3_frame_info.samprate);
              // ESP_LOGD(TAG, "Channels: %d", mp3_frame_info.nChans);
              // ESP_LOGD(TAG, "Bits Per Sample: %d", mp3_frame_info.bitsPerSample);
              // ESP_LOGD(TAG, "Bit Rate: %d", mp3_frame_info.bitrate);
              // }
              // size_t output_bytes = mp3_frame_info.outputSamps * sizeof(int16_t);

              // int bytes_per_sample = (this->mp3_frame_info_.bitsPerSample / 8) * this->mp3_frame_info_.nChans;
            }
          }
        }
      }
    }
    if (this_streamer->input_ring_buffer_->available() || this_streamer->output_ring_buffer_->available()) {
      event.type = EventType::RUNNING;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    } else {
      event.type = EventType::IDLE;
      xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
    }

    if (stopping && (this_streamer->input_ring_buffer_->available() == 0) &&
        (this_streamer->output_ring_buffer_->available() == 0)) {
      break;
    }
  }
  event.type = EventType::STOPPING;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  this_streamer->reset_ring_buffers();
  if (media_file_type == MediaFileType::MP3) {
    MP3FreeDecoder(mp3_decoder);
  }
  allocator.deallocate(buffer, BUFFER_SIZE);         // * sizeof(int16_t));
  allocator.deallocate(buffer_output, BUFFER_SIZE);  // * sizeof(int16_t));

  event.type = EventType::STOPPED;
  xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void DecodeStreamer::reset_ring_buffers() {
  this->input_ring_buffer_->reset();
  this->output_ring_buffer_->reset();
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
    // ESP_LOGE(TAG, "Failed to initialize HTTP connection");
    return MediaFileType::NONE;
  }

  esp_err_t err;
  if ((err = esp_http_client_open(*client, 0)) != ESP_OK) {
    // ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    this->cleanup_connection_(client);
    return MediaFileType::NONE;
  }

  int content_length = esp_http_client_fetch_headers(*client);

  if (content_length <= 0) {
    // ESP_LOGE(TAG, "Failed to get content length: %s", esp_err_to_name(err));
    this->cleanup_connection_(client);
    return MediaFileType::NONE;
  }

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
  }

  return MediaFileType::NONE;
}

void HTTPStreamer::start(const std::string &task_name, UBaseType_t priority) {
  if (this->task_handle_ == nullptr) {
    xTaskCreate(HTTPStreamer::read_task_, task_name.c_str(), 4096, (void *) this, priority, &this->task_handle_);
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
          this_streamer->reset_ring_buffers();
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
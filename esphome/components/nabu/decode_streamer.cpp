#ifdef USE_ESP_IDF

#include "decode_streamer.h"

#include "flac_decoder.h"
#include "mp3_decoder.h"
#include "streamer.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace nabu {

static const size_t BUFFER_SIZE = 4 * 8192;
static const size_t QUEUE_COUNT = 10;

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

  // event.type = EventType::STARTED;
  // xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  MediaFileType media_file_type = MediaFileType::NONE;

  // TODO: only initialize if needed
  HMP3Decoder mp3_decoder = MP3InitDecoder();

  flac::FLACDecoder flac_decoder = flac::FLACDecoder(buffer, BUFFER_SIZE, BUFFER_SIZE / 8);
  size_t flac_decoder_output_buffer_size = 0;
  size_t output_flac_bytes = 0;
  uint8_t *flac_buffer_current = buffer;
  uint8_t *flac_output_buffer_current = buffer_output;
  size_t flac_input_length = 0;

  MP3FrameInfo mp3_frame_info;
  int mp3_bytes_left = 0;

  uint8_t *mp3_buffer_current = buffer;

  int mp3_output_bytes_left = 0;
  uint8_t *mp3_output_buffer_current = buffer_output;

  bool stopping = false;
  bool header_parsed = false;

  StreamInfo stream_info;

  while (true) {
    if (xQueueReceive(this_streamer->command_queue_, &command_event, (0 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::START) {
        if ((media_file_type == MediaFileType::NONE) || (media_file_type == MediaFileType::MP3)) {
          MP3FreeDecoder(mp3_decoder);
        }

        // Set to nonsense... the decoder should update when the header is analyzed
        stream_info.channels = 0;

        // Reset state of everything
        this_streamer->reset_ring_buffers();
        memset((void *) buffer, 0, BUFFER_SIZE);
        memset((void *) buffer_output, 0, BUFFER_SIZE);

        mp3_bytes_left = 0;

        mp3_buffer_current = buffer;

        mp3_output_bytes_left = 0;
        mp3_output_buffer_current = buffer_output;

        stopping = false;
        header_parsed = false;

        size_t flac_decoder_output_buffer_size = 0;
        size_t output_flac_bytes = 0;
        uint8_t *flac_buffer_current = buffer;
        uint8_t *flac_output_buffer_current = buffer_output;
        size_t flac_input_length = 0;

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
      vTaskDelay(10 / portTICK_PERIOD_MS);
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

        StreamInfo old_stream_info = stream_info;

        stream_info.channels = 1;
        stream_info.sample_rate = 16000;

        if (stream_info != old_stream_info) {
          this_streamer->output_ring_buffer_->reset();

          event.type = EventType::STARTED;
          event.media_file_type = media_file_type;
          event.stream_info = stream_info;
          xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
        }
      }
      size_t bytes_written = 0;
      size_t bytes_read = 0;
    //   size_t bytes_to_read = std::min(max_bytes_to_read, BUFFER_SIZE);
      size_t bytes_to_read = std::min(this_streamer->output_ring_buffer_->free(), BUFFER_SIZE);
      if (max_bytes_to_read > 0) {
        bytes_read = this_streamer->input_ring_buffer_->read((void *) buffer, bytes_to_read, (10 / portTICK_PERIOD_MS));
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
        // size_t bytes_to_read = std::min(bytes_available, BUFFER_SIZE - mp3_bytes_left);
        size_t bytes_to_read = BUFFER_SIZE - mp3_bytes_left;
        if (bytes_to_read > 0) {
          uint8_t *new_mp3_data = buffer + mp3_bytes_left;
          bytes_read =
              this_streamer->input_ring_buffer_->read((void *) new_mp3_data, bytes_to_read, (10 / portTICK_PERIOD_MS));

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

            MP3GetLastFrameInfo(mp3_decoder, &mp3_frame_info);
            if (mp3_frame_info.outputSamps > 0) {
              int bytes_per_sample = (mp3_frame_info.bitsPerSample / 8);
              mp3_output_bytes_left = mp3_frame_info.outputSamps * bytes_per_sample;
              mp3_output_buffer_current = buffer_output;

              StreamInfo old_stream_info = stream_info;
              stream_info.sample_rate = mp3_frame_info.samprate;
              stream_info.channels = mp3_frame_info.nChans;
              stream_info.bits_per_sample = mp3_frame_info.bitsPerSample;

              if (stream_info != old_stream_info) {
                this_streamer->output_ring_buffer_->reset();

                event.type = EventType::STARTED;
                event.media_file_type = media_file_type;
                event.stream_info = stream_info;
                xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
              };
            }
          }
        }
      }
    } else if (media_file_type == MediaFileType::FLAC) {
      if (output_flac_bytes > 0) {
        size_t bytes_to_write = std::min(output_flac_bytes, this_streamer->output_ring_buffer_->free());
        if (bytes_to_write > 0) {
          size_t bytes_written =
              this_streamer->output_ring_buffer_->write((void *) flac_output_buffer_current, bytes_to_write);

          flac_output_buffer_current += bytes_written;
          output_flac_bytes -= bytes_written;
        }
      } else {
        if (flac_input_length > 0) {
          memmove((void *) buffer, (void *) flac_buffer_current, flac_input_length);
        }
        flac_buffer_current = buffer;

        // size_t bytes_to_read =
        //     std::min(this_streamer->input_ring_buffer_->available(), BUFFER_SIZE - flac_input_length);
        size_t bytes_to_read = BUFFER_SIZE - flac_input_length;

        if (bytes_to_read > 0) {
          uint8_t *new_flac_buffer_data = buffer + flac_input_length;
          size_t bytes_read = this_streamer->input_ring_buffer_->read((void *) new_flac_buffer_data, bytes_to_read,
                                                                      (10 / portTICK_PERIOD_MS));
          flac_input_length += bytes_read;
        }

        if (((flac_input_length > 0) && header_parsed) || (flac_input_length >8192)) {
          if (!header_parsed) {
            header_parsed = true;
            printf("flac_input_length=%d", flac_input_length);
            auto result = flac_decoder.read_header(flac_input_length);
            if (result != flac::FLAC_DECODER_SUCCESS) {
              printf("failed to read flac header %d", result);
              printf("%.*s", 4, (char *) buffer);
              break;
            } else {
                printf("successfully read flac header");
            }

            flac_input_length -= flac_decoder.get_bytes_index();
            flac_buffer_current += flac_decoder.get_bytes_index();

            StreamInfo old_stream_info = stream_info;

            stream_info.channels = flac_decoder.get_num_channels();
            stream_info.sample_rate = flac_decoder.get_sample_rate();
            stream_info.bits_per_sample = flac_decoder.get_sample_depth();

            if (stream_info != old_stream_info) {
              this_streamer->output_ring_buffer_->reset();

              event.type = EventType::STARTED;
              event.media_file_type = media_file_type;
              event.stream_info = stream_info;
              xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
            }

            flac_decoder_output_buffer_size = flac_decoder.get_output_buffer_size();
            if (BUFFER_SIZE < flac_decoder_output_buffer_size * sizeof(int16_t)) {
              printf("output buffer is not big enough");
              break;
            }
          } else {
            uint32_t output_samples = 0;
            auto result = flac_decoder.decode_frame((int16_t *) buffer_output, &output_samples, flac_input_length);

            if (result != flac::FLAC_DECODER_SUCCESS) {
              printf("failed to read flac file %d", result);
            //   printf("%.*s", 4, (char *) buffer);
              break;
            }

            flac_input_length -= flac_decoder.get_bytes_index();
            flac_buffer_current += flac_decoder.get_bytes_index();

            flac_output_buffer_current = buffer_output;
            output_flac_bytes = output_samples * sizeof(int16_t);
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

}  // namespace nabu
}  // namespace esphome
#endif
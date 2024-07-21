#ifdef USE_ESP_IDF

#include "decode_streamer.h"

#include "flac_decoder.h"
#include "mp3_decoder.h"
#include "streamer.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace nabu {

static const size_t BUFFER_SIZE = 4 * 8192;  // FLAC can require very large output buffers...
static const size_t QUEUE_COUNT = 20;

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
    xTaskCreate(DecodeStreamer::decode_task_, task_name.c_str(), 3072, (void *) this, priority, &this->task_handle_);
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
  uint8_t *input_buffer = allocator.allocate(BUFFER_SIZE);
  uint8_t *output_buffer = allocator.allocate(BUFFER_SIZE);

  uint8_t *input_buffer_current = input_buffer;
  uint8_t *output_buffer_current = output_buffer;

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

  // event.type = EventType::STARTED;
  // xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);

  MediaFileType media_file_type = MediaFileType::NONE;

  // TODO: only initialize if needed
  HMP3Decoder mp3_decoder = MP3InitDecoder();
  MP3FrameInfo mp3_frame_info;

  flac::FLACDecoder flac_decoder =
      flac::FLACDecoder(input_buffer, BUFFER_SIZE, BUFFER_SIZE / 8, this_streamer->input_ring_buffer_.get());
  size_t flac_decoder_output_buffer_min_size = 0;

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
        memset((void *) input_buffer, 0, BUFFER_SIZE);
        memset((void *) output_buffer, 0, BUFFER_SIZE);

        input_buffer_length = 0;
        output_buffer_length = 0;
        input_buffer_current = input_buffer;
        output_buffer_current = output_buffer;

        stopping = false;
        header_parsed = false;

        flac_decoder_output_buffer_min_size = 0;

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

    if (output_buffer_length > 0) {
      size_t bytes_free = this_streamer->output_ring_buffer_->free();
      size_t bytes_to_write = std::min(output_buffer_length, bytes_free);

      if (bytes_to_write > 0) {
        size_t bytes_written =
            this_streamer->output_ring_buffer_->write((void *) output_buffer_current, bytes_to_write);

        output_buffer_length -= bytes_written;
        output_buffer_current += bytes_written;
      }
    } else {
      if (media_file_type == MediaFileType::WAV) {
        size_t bytes_available = this_streamer->input_ring_buffer_->available();
        size_t bytes_free = this_streamer->output_ring_buffer_->free();
        size_t max_bytes_to_read = std::min(bytes_free, bytes_available);

        size_t bytes_read = 0;

        if (!header_parsed) {
          header_parsed = true;
          bytes_read = this_streamer->input_ring_buffer_->read((void *) input_buffer, 44);
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

        size_t bytes_to_read = std::min(max_bytes_to_read, BUFFER_SIZE);
        if (max_bytes_to_read > 0) {
          bytes_read =
              this_streamer->input_ring_buffer_->read((void *) input_buffer, bytes_to_read, (10 / portTICK_PERIOD_MS));
        }

        if (bytes_read > 0) {
          bytes_written = this_streamer->output_ring_buffer_->write((void *) input_buffer, bytes_read);
        }
      } else if (media_file_type == MediaFileType::MP3) {
        // Shift unread data in buffer to start
        if ((input_buffer_length > 0) && (input_buffer_length < BUFFER_SIZE)) {
          memmove(input_buffer, input_buffer_current, input_buffer_length);
        }
        input_buffer_current = input_buffer;

        // read in new mp3 data to fill the buffer
        size_t bytes_available = this_streamer->input_ring_buffer_->available();
        size_t bytes_to_read = std::min(bytes_available, BUFFER_SIZE - input_buffer_length);
        size_t bytes_read = 0;

        if (bytes_to_read > 0) {
          uint8_t *new_mp3_data = input_buffer + input_buffer_length;
          bytes_read =
              this_streamer->input_ring_buffer_->read((void *) new_mp3_data, bytes_to_read, (10 / portTICK_PERIOD_MS));

          input_buffer_length += bytes_read;
        }

        if (input_buffer_length > 0) {
          // Look for the next sync word
          int32_t offset = MP3FindSyncWord(input_buffer_current, input_buffer_length);
          if (offset < 0) {
            event.type = EventType::WARNING;
            event.err = ESP_ERR_NO_MEM;
            xQueueSend(this_streamer->event_queue_, &event, portMAX_DELAY);
            continue;
          }

          // Advance read pointer
          input_buffer_current += offset;
          input_buffer_length -= offset;

          int err =
              MP3Decode(mp3_decoder, &input_buffer_current, (int *) &input_buffer_length, (int16_t *) output_buffer, 0);
          if (err) {
            switch (err) {
              case ERR_MP3_MAINDATA_UNDERFLOW:
                // Not a problem. Next call to decode will provide more data.
                continue;
                break;
              case ERR_MP3_INDATA_UNDERFLOW:
                // TODO: Better handle mp3 decoder errors
                break;
              default:
                // TODO: Better handle mp3 decoder errors
                break;
            }
          } else {
            MP3GetLastFrameInfo(mp3_decoder, &mp3_frame_info);
            if (mp3_frame_info.outputSamps > 0) {
              int bytes_per_sample = (mp3_frame_info.bitsPerSample / 8);
              output_buffer_length = mp3_frame_info.outputSamps * bytes_per_sample;
              output_buffer_current = output_buffer;

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
      } else if (media_file_type == MediaFileType::FLAC) {
        if (!header_parsed) {
          if (this_streamer->input_ring_buffer_->available() > 0) {
            auto result = flac_decoder.read_header();

            if (result != flac::FLAC_DECODER_SUCCESS) {
              printf("failed to read flac header. Error: %d\n", result);
              break;
            }

            input_buffer_length -= flac_decoder.get_bytes_index();
            input_buffer_current += flac_decoder.get_bytes_index();

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

            flac_decoder_output_buffer_min_size = flac_decoder.get_output_buffer_size();
            if (BUFFER_SIZE < flac_decoder_output_buffer_min_size * sizeof(int16_t)) {
              printf("output buffer is not big enough");
              break;
            }
            header_parsed = true;
          }

        } else {
          if (this_streamer->input_ring_buffer_->available() == 0) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
          }
          uint32_t output_samples = 0;
          auto result = flac_decoder.decode_frame((int16_t *) output_buffer, &output_samples);

          if (result != flac::FLAC_DECODER_SUCCESS) {
            break;
          }

          output_buffer_current = output_buffer;
          output_buffer_length = output_samples * sizeof(int16_t);
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
  flac_decoder.free_buffers();
  allocator.deallocate(input_buffer, BUFFER_SIZE);
  allocator.deallocate(output_buffer, BUFFER_SIZE);

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
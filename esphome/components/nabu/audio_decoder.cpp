#ifdef USE_ESP_IDF

#include "audio_decoder.h"

#include "mp3_decoder.h"

#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace nabu {

AudioDecoder::AudioDecoder(RingBuffer *input_ring_buffer, RingBuffer *output_ring_buffer, size_t internal_buffer_size) {
  this->input_ring_buffer_ = input_ring_buffer;
  this->output_ring_buffer_ = output_ring_buffer;
  this->internal_buffer_size_ = internal_buffer_size;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->input_buffer_ = allocator.allocate(internal_buffer_size);
  this->output_buffer_ = allocator.allocate(internal_buffer_size);
}

AudioDecoder::~AudioDecoder() {
  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  if (this->input_buffer_ != nullptr) {
    allocator.deallocate(this->input_buffer_, this->internal_buffer_size_);
  }
  if (this->output_buffer_ != nullptr) {
    allocator.deallocate(this->output_buffer_, this->internal_buffer_size_);
  }

  if (this->flac_decoder_ != nullptr) {
    this->flac_decoder_->free_buffers();
    delete this->flac_decoder_;
    this->flac_decoder_ = nullptr;
  }

  if (this->wav_decoder_ != nullptr) {
    delete this->wav_decoder_;
    this->wav_decoder_ = nullptr;
  }

  if (this->media_file_type_ == media_player::MediaFileType::MP3) {
    MP3FreeDecoder(this->mp3_decoder_);
  }
}

void AudioDecoder::start(media_player::MediaFileType media_file_type) {
  this->media_file_type_ = media_file_type;

  this->input_buffer_current_ = this->input_buffer_;
  this->input_buffer_length_ = 0;
  this->output_buffer_current_ = this->output_buffer_;
  this->output_buffer_length_ = 0;

  this->potentially_failed_count_ = 0;
  this->end_of_file_ = false;

  switch (this->media_file_type_) {
    case media_player::MediaFileType::WAV:
      this->wav_decoder_ = new wav_decoder::WAVDecoder(&this->input_buffer_current_);
      this->wav_decoder_->reset();
      break;
    case media_player::MediaFileType::MP3:
      this->mp3_decoder_ = MP3InitDecoder();
      break;
    case media_player::MediaFileType::FLAC:
      this->flac_decoder_ = new flac::FLACDecoder(this->input_buffer_);
      break;
    case media_player::MediaFileType::NONE:
      break;
  }
}

AudioDecoderState AudioDecoder::decode(bool stop_gracefully) {
  if (stop_gracefully) {
    if (this->output_buffer_length_ == 0) {
      // If the file decoder believes it the end of file
      if (this->end_of_file_) {
        return AudioDecoderState::FINISHED;
      }
      // If all the internal buffers are empty, the decoding is done
      if ((this->input_ring_buffer_->available() == 0) && (this->input_buffer_length_ == 0)) {
        return AudioDecoderState::FINISHED;
      }
    }
  }

  if (this->potentially_failed_count_ > 5) {
    return AudioDecoderState::FAILED;
  }

  FileDecoderState state = FileDecoderState::MORE_TO_PROCESS;

  while (state == FileDecoderState::MORE_TO_PROCESS) {
    if (this->output_buffer_length_ > 0) {
      // Have decoded data, feed into output ring buffer
      size_t bytes_free = this->output_ring_buffer_->free();
      size_t bytes_to_write = std::min(this->output_buffer_length_, bytes_free);

      if (bytes_to_write > 0) {
        size_t bytes_written = this->output_ring_buffer_->write((void *) this->output_buffer_current_, bytes_to_write);

        this->output_buffer_length_ -= bytes_written;
        this->output_buffer_current_ += bytes_written;
      }

      if (this->output_buffer_length_ > 0) {
        // Output ring buffer is full, so we can't do any more processing
        return AudioDecoderState::DECODING;
      }
    } else {
      // Try to decode more data
      size_t bytes_available = this->input_ring_buffer_->available();
      size_t bytes_to_read = std::min(bytes_available, this->internal_buffer_size_ - this->input_buffer_length_);

      if ((this->potentially_failed_count_ > 0) && (bytes_to_read == 0)) {
        // We didn't have enough data last time, and we have no new data, so just return
        return AudioDecoderState::DECODING;
      }

      // Shift unread data in input buffer to start
      if (this->input_buffer_length_ > 0) {
        memmove(this->input_buffer_, this->input_buffer_current_, this->input_buffer_length_);
      }
      this->input_buffer_current_ = this->input_buffer_;

      // read in new ring buffer data to fill the remaining input buffer
      size_t bytes_read = 0;

      if (bytes_to_read > 0) {
        uint8_t *new_audio_data = this->input_buffer_ + this->input_buffer_length_;
        bytes_read = this->input_ring_buffer_->read((void *) new_audio_data, bytes_to_read);

        this->input_buffer_length_ += bytes_read;
      }

      if (this->input_buffer_length_ == 0) {
        // No input data available, so we can't do any more processing
        state = FileDecoderState::IDLE;
      } else {
        switch (this->media_file_type_) {
          case media_player::MediaFileType::WAV:
            state = this->decode_wav_();
            break;
          case media_player::MediaFileType::MP3:
            state = this->decode_mp3_();
            break;
          case media_player::MediaFileType::FLAC:
            state = this->decode_flac_();
            break;
          case media_player::MediaFileType::NONE:
            state = FileDecoderState::IDLE;
            break;
        }
      }
    }
    if (state == FileDecoderState::POTENTIALLY_FAILED) {
      ++this->potentially_failed_count_;
    } else if (state == FileDecoderState::END_OF_FILE) {
      this->end_of_file_ = true;
    } else if (state == FileDecoderState::FAILED) {
      return AudioDecoderState::FAILED;
    } else {
      this->potentially_failed_count_ = 0;
    }
  }
  return AudioDecoderState::DECODING;
}

FileDecoderState AudioDecoder::decode_wav_() {
  if (!this->stream_info_.has_value() && (this->input_buffer_length_ > 44)) {
    // Header hasn't been processed

    size_t original_buffer_length = this->input_buffer_length_;

    size_t wav_bytes_to_skip = this->wav_decoder_->bytes_to_skip();
    size_t wav_bytes_to_read = this->wav_decoder_->bytes_needed();

    bool header_finished = false;
    while (!header_finished) {
      if (wav_bytes_to_skip > 0) {
        // Adjust pointer to skip the appropriate bytes
        this->input_buffer_current_ += wav_bytes_to_skip;
        this->input_buffer_length_ -= wav_bytes_to_skip;
        wav_bytes_to_skip = 0;
      } else if (wav_bytes_to_read > 0) {
        wav_decoder::WAVDecoderResult result = this->wav_decoder_->next();
        this->input_buffer_current_ += wav_bytes_to_read;
        this->input_buffer_length_ -= wav_bytes_to_read;

        if (result == wav_decoder::WAV_DECODER_SUCCESS_IN_DATA) {
          // Header parsing is complete

          // Assume PCM
          media_player::StreamInfo stream_info;
          stream_info.channels = this->wav_decoder_->num_channels();
          stream_info.sample_rate = this->wav_decoder_->sample_rate();
          stream_info.bits_per_sample = this->wav_decoder_->bits_per_sample();
          this->stream_info_ = stream_info;

          printf("sample channels: %d\n", this->stream_info_.value().channels);
          printf("sample rate: %" PRId32 "\n", this->stream_info_.value().sample_rate);
          printf("bits per sample: %d\n", this->stream_info_.value().bits_per_sample);
          this->wav_bytes_left_ = this->wav_decoder_->chunk_bytes_left();
          header_finished = true;
        } else if (result == wav_decoder::WAV_DECODER_SUCCESS_NEXT) {
          // Continue parsing header
          wav_bytes_to_skip = this->wav_decoder_->bytes_to_skip();
          wav_bytes_to_read = this->wav_decoder_->bytes_needed();
        } else {
          printf("Unexpected error while parsing WAV header: %d\n", result);
          return FileDecoderState::FAILED;
        }
      } else {
        // Something unexpected has happened
        // Reset state and hope we have enough info next time
        this->input_buffer_length_ = original_buffer_length;
        this->input_buffer_current_ = this->input_buffer_;
        return FileDecoderState::POTENTIALLY_FAILED;
      }
    }
  }

  if (this->wav_bytes_left_ > 0) {
    size_t bytes_to_write = std::min(this->wav_bytes_left_, this->input_buffer_length_);
    bytes_to_write = std::min(bytes_to_write, this->internal_buffer_size_);
    if (bytes_to_write > 0) {
      std::memcpy(this->output_buffer_, this->input_buffer_current_, bytes_to_write);
      this->input_buffer_current_ += bytes_to_write;
      this->input_buffer_length_ -= bytes_to_write;
      this->output_buffer_current_ = this->output_buffer_;
      this->output_buffer_length_ = bytes_to_write;
      this->wav_bytes_left_ -= bytes_to_write;
    }

    return FileDecoderState::IDLE;
  }

  return FileDecoderState::END_OF_FILE;
}

FileDecoderState AudioDecoder::decode_mp3_() {
  // Look for the next sync word
  int32_t offset = MP3FindSyncWord(this->input_buffer_current_, this->input_buffer_length_);
  if (offset < 0) {
    // We may recover if we have more data
    return FileDecoderState::POTENTIALLY_FAILED;
  }

  // Advance read pointer
  this->input_buffer_current_ += offset;
  this->input_buffer_length_ -= offset;

  int err = MP3Decode(this->mp3_decoder_, &this->input_buffer_current_, (int *) &this->input_buffer_length_,
                      (int16_t *) this->output_buffer_, 0);
  if (err) {
    switch (err) {
      case ERR_MP3_MAINDATA_UNDERFLOW:
        // Not a problem. Next call to decode will provide more data.
        return FileDecoderState::POTENTIALLY_FAILED;
        break;
      default:
        // TODO: Better handle mp3 decoder errors
        return FileDecoderState::FAILED;
        break;
    }
  } else {
    MP3FrameInfo mp3_frame_info;
    MP3GetLastFrameInfo(this->mp3_decoder_, &mp3_frame_info);
    if (mp3_frame_info.outputSamps > 0) {
      int bytes_per_sample = (mp3_frame_info.bitsPerSample / 8);
      this->output_buffer_length_ = mp3_frame_info.outputSamps * bytes_per_sample;
      this->output_buffer_current_ = this->output_buffer_;

      media_player::StreamInfo stream_info;
      stream_info.channels = mp3_frame_info.nChans;
      stream_info.sample_rate = mp3_frame_info.samprate;
      stream_info.bits_per_sample = mp3_frame_info.bitsPerSample;
      this->stream_info_ = stream_info;
    }
  }
  // }
  return FileDecoderState::MORE_TO_PROCESS;
}

FileDecoderState AudioDecoder::decode_flac_() {
  if (!this->stream_info_.has_value()) {
    // Header hasn't been read
    auto result = this->flac_decoder_->read_header(this->input_buffer_length_);

    size_t bytes_consumed = this->flac_decoder_->get_bytes_index();
    this->input_buffer_current_ += bytes_consumed;
    this->input_buffer_length_ = this->flac_decoder_->get_bytes_left();

    if (result == flac::FLAC_DECODER_HEADER_OUT_OF_DATA) {
      return FileDecoderState::POTENTIALLY_FAILED;
    }

    if (result != flac::FLAC_DECODER_SUCCESS) {
      printf("failed to read flac header. Error: %d\n", result);
      return FileDecoderState::FAILED;
    }

    media_player::StreamInfo stream_info;
    stream_info.channels = this->flac_decoder_->get_num_channels();
    stream_info.sample_rate = this->flac_decoder_->get_sample_rate();
    stream_info.bits_per_sample = this->flac_decoder_->get_sample_depth();

    size_t flac_decoder_output_buffer_min_size = flac_decoder_->get_output_buffer_size();
    if (this->internal_buffer_size_ < flac_decoder_output_buffer_min_size * sizeof(int16_t)) {
      printf("output buffer is not big enough\n");
      return FileDecoderState::FAILED;
    }

    return FileDecoderState::MORE_TO_PROCESS;
  }

  uint32_t output_samples = 0;
  auto result =
      this->flac_decoder_->decode_frame(this->input_buffer_length_, (int16_t *) this->output_buffer_, &output_samples);

  if (result == flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
    // Not an issue, just needs more data that we'll get next time.
    return FileDecoderState::POTENTIALLY_FAILED;
  } else if (result > flac::FLAC_DECODER_ERROR_OUT_OF_DATA) {
    // Serious error, can't recover
    printf("FLAC Decoder Error %d\n", result);
    return FileDecoderState::FAILED;
  }

  // We have successfully decoded some input data and have new output data
  size_t bytes_consumed = this->flac_decoder_->get_bytes_index();
  this->input_buffer_current_ += bytes_consumed;
  this->input_buffer_length_ = this->flac_decoder_->get_bytes_left();

  this->output_buffer_current_ = this->output_buffer_;
  this->output_buffer_length_ = output_samples * sizeof(int16_t);

  if (result == flac::FLAC_DECODER_NO_MORE_FRAMES) {
    return FileDecoderState::END_OF_FILE;
  }

  return FileDecoderState::MORE_TO_PROCESS;
}

}  // namespace nabu
}  // namespace esphome

#endif
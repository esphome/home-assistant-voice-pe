#ifdef USE_ESP_IDF
#include "wav_decoder.h"

namespace wav_decoder {

WAVDecoderResult WAVDecoder::next() {
  this->bytes_to_skip_ = 0;

  switch (this->state_) {
    case WAV_DECODER_BEFORE_RIFF: {
      this->chunk_name_ = std::string((const char *) *this->buffer_, 4);
      if (this->chunk_name_ != "RIFF") {
        return WAV_DECODER_ERROR_NO_RIFF;
      }

      this->chunk_bytes_left_ = *((uint32_t *) (*this->buffer_ + 4));
      if ((this->chunk_bytes_left_ % 2) != 0) {
        // Pad byte
        this->chunk_bytes_left_++;
      }

      // WAVE sub-chunk header should follow
      this->state_ = WAV_DECODER_BEFORE_WAVE;
      this->bytes_needed_ = 4;  // WAVE
      break;
    }

    case WAV_DECODER_BEFORE_WAVE: {
      this->chunk_name_ = std::string((const char *) *this->buffer_, 4);
      if (this->chunk_name_ != "WAVE") {
        return WAV_DECODER_ERROR_NO_WAVE;
      }

      // Next chunk header
      this->state_ = WAV_DECODER_BEFORE_FMT;
      this->bytes_needed_ = 8;  // chunk name + size
      break;
    }

    case WAV_DECODER_BEFORE_FMT: {
      this->chunk_name_ = std::string((const char *) *this->buffer_, 4);
      this->chunk_bytes_left_ = *((uint32_t *) (*this->buffer_ + 4));
      if ((this->chunk_bytes_left_ % 2) != 0) {
        // Pad byte
        this->chunk_bytes_left_++;
      }

      if (this->chunk_name_ == "fmt ") {
        // Read rest of fmt chunk
        this->state_ = WAV_DECODER_IN_FMT;
        this->bytes_needed_ = this->chunk_bytes_left_;
      } else {
        // Skip over chunk
        // this->state_ = WAV_DECODER_BEFORE_FMT_SKIP_CHUNK;
        this->bytes_to_skip_ = this->chunk_bytes_left_;
        this->bytes_needed_ = 8;
      }
      break;
    }

      // case WAV_DECODER_BEFORE_FMT_SKIP_CHUNK: {
      //   // Next chunk header
      //   this->state_ = WAV_DECODER_BEFORE_FMT;
      //   this->bytes_needed_ = 8; // chunk name + size
      //   break;
      // }

    case WAV_DECODER_IN_FMT: {
      /**
       * audio format (uint16_t)
       * number of channels (uint16_t)
       * sample rate (uint32_t)
       * bytes per second (uint32_t)
       * block align (uint16_t)
       * bits per sample (uint16_t)
       * [rest of format chunk]
       */
      this->num_channels_ = *((uint16_t *) (*this->buffer_ + 2));
      this->sample_rate_ = *((uint32_t *) (*this->buffer_ + 4));
      this->bits_per_sample_ = *((uint16_t *) (*this->buffer_ + 14));

      // Next chunk
      this->state_ = WAV_DECODER_BEFORE_DATA;
      this->bytes_needed_ = 8;  // chunk name + size
      break;
    }

    case WAV_DECODER_BEFORE_DATA: {
      this->chunk_name_ = std::string((const char *) *this->buffer_, 4);
      this->chunk_bytes_left_ = *((uint32_t *) (*this->buffer_ + 4));
      if ((this->chunk_bytes_left_ % 2) != 0) {
        // Pad byte
        this->chunk_bytes_left_++;
      }

      if (this->chunk_name_ == "data") {
        // Complete
        this->state_ = WAV_DECODER_IN_DATA;
        this->bytes_needed_ = 0;
        return WAV_DECODER_SUCCESS_IN_DATA;
      }

      // Skip over chunk
      // this->state_ = WAV_DECODER_BEFORE_DATA_SKIP_CHUNK;
      this->bytes_to_skip_ = this->chunk_bytes_left_;
      this->bytes_needed_ = 8;
      break;
    }

      // case WAV_DECODER_BEFORE_DATA_SKIP_CHUNK: {
      //   // Next chunk header
      //   this->state_ = WAV_DECODER_BEFORE_DATA;
      //   this->bytes_needed_ = 8; // chunk name + size
      //   break;
      // }

    case WAV_DECODER_IN_DATA: {
      return WAV_DECODER_SUCCESS_IN_DATA;
      break;
    }
  }

  return WAV_DECODER_SUCCESS_NEXT;
}

}  // namespace wav_decoder
#endif
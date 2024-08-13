#ifdef USE_ESP_IDF

// Very basic WAV file decoder that parses format information and gets to the
// data portion of the file.
// Skips over extraneous chunks like LIST and INFO.

#ifndef WAV_DECODER_H_
#define WAV_DECODER_H_

#include <cstdint>
#include <string>

/* WAV header:
 * 'RIFF' (4 bytes, ASCII)
 * RIFF chunk size (uint32_t)
 * 'WAVE' (4 bytes, ASCII)
 * (optional RIFF chunks)
 * 'fmt ' (4 bytes, ASCII)
 * format chunk size (uint32_t)
 * audio format (uint16_t, PCM = 1)
 * number of channels (uint16_t)
 * sample rate (uint32_t)
 * bytes per second (uint32_t)
 * block align (uint16_t)
 * bits per sample (uint16_t)
 * [rest of format chunk]
 * (optional RIFF chunks)
 * 'data' (4 bytes, ASCII)
 * data chunks size (uint32_t)
 * [rest of data chunk]
 * (optional RIFF chunks)
 * */

namespace wav_decoder {

const std::size_t min_buffer_size = 24;

enum WAVDecoderState {

  WAV_DECODER_BEFORE_RIFF = 0,
  WAV_DECODER_BEFORE_WAVE = 1,
  WAV_DECODER_BEFORE_FMT = 2,
  WAV_DECODER_IN_FMT = 3,
  WAV_DECODER_BEFORE_DATA = 4,
  WAV_DECODER_IN_DATA = 5,

};

enum WAVDecoderResult {
  WAV_DECODER_SUCCESS_NEXT = 0,
  WAV_DECODER_SUCCESS_IN_DATA = 1,
  WAV_DECODER_ERROR_NO_RIFF = 2,
  WAV_DECODER_ERROR_NO_WAVE = 3,
};

class WAVDecoder {
 public:
  WAVDecoder(uint8_t **buffer) : buffer_(buffer) {};
  ~WAVDecoder() {};

  WAVDecoderState state() { return this->state_; }
  std::size_t bytes_to_skip() { return this->bytes_to_skip_; }
  std::size_t bytes_needed() { return this->bytes_needed_; }
  std::string chunk_name() { return this->chunk_name_; }
  std::size_t chunk_bytes_left() { return this->chunk_bytes_left_; }
  uint32_t sample_rate() { return this->sample_rate_; }
  uint16_t num_channels() { return this->num_channels_; }
  uint16_t bits_per_sample() { return this->bits_per_sample_; }

  // Advance decoding:
  // 1. Check bytes_to_skip() first, and skip that many bytes.
  // 2. Read exactly bytes_needed() into the start of the buffer.
  // 3. Run next() and loop to 1 until the result is
  // WAV_DECODER_SUCCESS_IN_DATA.
  // 4. Use chunk_bytes_left() to read the data samples.
  WAVDecoderResult next();

  void reset() {
    this->state_ = WAV_DECODER_BEFORE_RIFF;
    this->bytes_to_skip_ = 0;
    this->chunk_name_ = "";
    this->chunk_bytes_left_ = 0;

    this->sample_rate_ = 0;
    this->num_channels_ = 0;
    this->bits_per_sample_ = 0;
  }

 protected:
  uint8_t **buffer_;
  WAVDecoderState state_ = WAV_DECODER_BEFORE_RIFF;
  std::size_t bytes_needed_ = 8;  // chunk name + size
  std::size_t bytes_to_skip_ = 0;
  std::string chunk_name_;
  std::size_t chunk_bytes_left_ = 0;

  uint32_t sample_rate_ = 0;
  uint16_t num_channels_ = 0;
  uint16_t bits_per_sample_ = 0;
};
}  // namespace wav_decoder

#endif  // WAV_DECODER_H_
#endif
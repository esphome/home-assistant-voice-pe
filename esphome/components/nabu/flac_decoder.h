// Port of:
// https://www.nayuki.io/res/simple-flac-implementation/simple-decode-flac-to-wav.py
//
// Uses some small parts from: https://github.com/schreibfaul1/ESP32-audioI2S/
// See also: https://xiph.org/flac/format.html

#ifdef USE_ESP_IDF

#ifndef _FLAC_DECODER_H
#define _FLAC_DECODER_H

#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include <cstdint>

namespace flac {

// 'fLaC'
const static uint32_t FLAC_MAGIC_NUMBER = 0x664C6143;

const static uint32_t FLAC_UINT_MASK[] = {
    0x00000000, 0x00000001, 0x00000003, 0x00000007, 0x0000000f, 0x0000001f, 0x0000003f, 0x0000007f, 0x000000ff,
    0x000001ff, 0x000003ff, 0x000007ff, 0x00000fff, 0x00001fff, 0x00003fff, 0x00007fff, 0x0000ffff, 0x0001ffff,
    0x0003ffff, 0x0007ffff, 0x000fffff, 0x001fffff, 0x003fffff, 0x007fffff, 0x00ffffff, 0x01ffffff, 0x03ffffff,
    0x07ffffff, 0x0fffffff, 0x1fffffff, 0x3fffffff, 0x7fffffff, 0xffffffff};

enum FLACDecoderResult {
  FLAC_DECODER_SUCCESS = 0,
  FLAC_DECODER_NO_MORE_FRAMES = 1,
  FLAC_DECODER_HEADER_OUT_OF_DATA = 2,
  FLAC_DECODER_ERROR_OUT_OF_DATA = 3,
  FLAC_DECODER_ERROR_BAD_MAGIC_NUMBER = 4,
  FLAC_DECODER_ERROR_SYNC_NOT_FOUND = 5,
  FLAC_DECODER_ERROR_BAD_BLOCK_SIZE_CODE = 6,
  FLAC_DECODER_ERROR_BAD_HEADER = 7,
  FLAC_DECODER_ERROR_RESERVED_CHANNEL_ASSIGNMENT = 8,
  FLAC_DECODER_ERROR_RESERVED_SUBFRAME_TYPE = 9,
  FLAC_DECODER_ERROR_BAD_FIXED_PREDICTION_ORDER = 10,
  FLAC_DECODER_ERROR_RESERVED_RESIDUAL_CODING_METHOD = 11,
  FLAC_DECODER_ERROR_BLOCK_SIZE_NOT_DIVISIBLE_RICE = 12,
  FLAC_DECODER_ERROR_MEMORY_ALLOCATION_ERROR = 13,
  FLAC_DECODER_ERROR_BLOCK_SIZE_OUT_OF_RANGE = 14
};

// Coefficients for fixed linear prediction
const static std::vector<int16_t> FLAC_FIXED_COEFFICIENTS[] = {
    {1}, {1, 1}, {-1, 2, 1}, {1, -3, 3, 1}, {-1, 4, -6, 4, 1}};

/* Basic FLAC decoder ported from:
 * https://www.nayuki.io/res/simple-flac-implementation/simple-decode-flac-to-wav.py
 */
class FLACDecoder {
 public:
  /* buffer - FLAC data
   * buffer_size - size of the data buffer
   * min_buffer_size - min bytes in buffer before fill_buffer is called
   */
  FLACDecoder(uint8_t *buffer) : buffer_(buffer) {}

  ~FLACDecoder() { this->free_buffers(); }

  /* Reads FLAC header from buffer.
   * Must be called before decode_frame. */
  FLACDecoderResult read_header(size_t buffer_length);

  /* Decodes a single frame of audio.
   * Copies num_samples into output_buffer.
   * Use get_output_buffer_size() to allocate output_buffer. */
  FLACDecoderResult decode_frame(size_t buffer_length, int16_t *output_buffer, uint32_t *num_samples);

  /* Frees internal memory. */
  void free_buffers();

  /* Sample rate (after read_header()) */
  uint32_t get_sample_rate() { return this->sample_rate_; }

  /* Sample depth (after read_header()) */
  uint32_t get_sample_depth() { return this->sample_depth_; }

  /* Number of audio channels (after read_header()) */
  uint32_t get_num_channels() { return this->num_channels_; }

  /* Number of audio samples (after read_header()) */
  uint32_t get_num_samples() { return this->num_samples_; }

  /* Number of audio samples (after read_header()) */
  uint32_t get_min_block_size() { return this->min_block_size_; }
  /* Number of audio samples (after read_header()) */
  uint32_t get_max_block_size() { return this->max_block_size_; }
  
  
  /* Maximum number of output samples per frame (after read_header()) */
  uint32_t get_output_buffer_size() { return this->max_block_size_ * this->num_channels_; }

  /* Maximum number of output samples per frame (after read_header()) */
  uint32_t get_output_buffer_size_bytes() { return this->max_block_size_ * this->num_channels_ * this->sample_depth_ / 8; }

  std::size_t get_bytes_index() { return this->buffer_index_; }

  /* Number of unread bytes in the input buffer. */
  std::size_t get_bytes_left() { return this->bytes_left_; }

 protected:
  FLACDecoderResult frame_sync_();
  
  FLACDecoderResult decode_frame_header_();
  
  /* Decodes one or more subframes by type. */
  FLACDecoderResult decode_subframes(uint32_t block_size, uint32_t sample_depth, uint32_t channel_assignment);

  /* Decodes a subframe by type. */
  FLACDecoderResult decode_subframe(uint32_t block_size, uint32_t sample_depth, std::size_t block_samples_offset);

  /* Decodes a subframe with fixed coefficients. */
  FLACDecoderResult decode_fixed_subframe(uint32_t block_size, std::size_t block_samples_offset, uint32_t pre_order,
                                          uint32_t sample_depth);

  /* Decodes a subframe with dynamic coefficients. */
  FLACDecoderResult decode_lpc_subframe(uint32_t block_size, std::size_t block_samples_offset, uint32_t lpc_order,
                                        uint32_t sample_depth);

  /* Decodes prediction residuals. */
  FLACDecoderResult decode_residuals(int32_t* buffer, size_t warm_up_samples, uint32_t block_size);

  /* Completes predicted samples. */
  void restore_linear_prediction(int32_t* sub_frame_buffer, size_t num_of_samples, const std::vector<int16_t> &coefs, int32_t shift);

  bool wait_for_bytes_(uint32_t num_of_bytes, TickType_t ticks_to_wait );
  
  uint32_t read_aligned_byte();

  /* Reads an unsigned integer of arbitrary bit size. */
  uint32_t read_uint(std::size_t num_bits);

  /* Reads a singed integer of arbitrary bit size. */
  int32_t read_sint(std::size_t num_bits);

  /* Reads a rice-encoded signed integer. */
  int64_t read_rice_sint(uint8_t param);

  /* Forces input buffer to be byte-aligned. */
  void align_to_byte();

 private:
  /* Pointer to input buffer with FLAC data. */
  uint8_t *buffer_ = nullptr;

  /* Next index to read from the input buffer. */
  std::size_t buffer_index_ = 0;

  /* Number of byte that haven't been read from the input buffer yet. */
  std::size_t bytes_left_ = 0;

  /* Number of bits in the bit buffer. */
  std::size_t bit_buffer_length_ = 0;

  /* Last read bits from input buffer. */
  uint64_t bit_buffer_ = 0;

  /* True if input buffer is empty and cannot be filled. */
  bool out_of_data_ = false;

  /* Minimum number of samples in a block (single channel). */
  uint32_t min_block_size_ = 0;

  /* Maximum number of samples in a block (single channel). */
  uint32_t max_block_size_ = 0;

  uint32_t curr_frame_block_size_ = 0;
  uint32_t curr_frame_channel_assign_ = 0;
  
  /* Sample rate in hertz. */
  uint32_t sample_rate_ = 0;

  /* Number of audio channels. */
  uint32_t num_channels_ = 0;

  /* Bits per sample. */
  uint32_t sample_depth_ = 0;

  /* Total number of samples in the stream. */
  uint32_t num_samples_ = 0;

  /* Buffer of decoded samples at full precision (all channels). */
  int32_t *block_samples_ = nullptr;

  bool partial_header_read_{false};
  bool partial_header_last_{false};
  uint32_t partial_header_type_{0};
  uint32_t partial_header_length_{0};

  bool frame_sync_found_{false};
  uint8_t frame_sync_bytes_[2];
};

}  // namespace flac

#endif
#endif
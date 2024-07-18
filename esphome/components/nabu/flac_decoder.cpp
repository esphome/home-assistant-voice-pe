#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "flac_decoder.h"

namespace flac {

FLACDecoderResult FLACDecoder::read_header(size_t bytes_left) {
  
  this->bytes_left_ = bytes_left;
  this->buffer_index_ = 0;

  if (bytes_left == 0) {
    return FLAC_DECODER_ERROR_OUT_OF_DATA;
  }
  // if (this->out_of_data_) {
  //   return FLAC_DECODER_ERROR_OUT_OF_DATA;
  // }

  // if (this->bytes_left_ < this->min_buffer_size_) {
  //   // Refill the buffer before reading the header
  //   this->fill_buffer();
  // }

  // File must start with 'fLaC'
  if (this->read_uint(32) != FLAC_MAGIC_NUMBER) {
    return FLAC_DECODER_ERROR_BAD_MAGIC_NUMBER;
  }

  // Read header blocks
  bool last = false;
  uint32_t type = 0;
  uint32_t length = 0;

  while (!last) {
    if (this->out_of_data_) {
      return FLAC_DECODER_ERROR_OUT_OF_DATA;
    }

    last = this->read_uint(1) != 0;
    type = this->read_uint(7);
    length = this->read_uint(24);
    if (type == 0) {
      // Stream info block
      this->min_block_size_ = this->read_uint(16);
      this->max_block_size_ = this->read_uint(16);
      this->read_uint(24);
      this->read_uint(24);

      this->sample_rate_ = this->read_uint(20);
      this->num_channels_ = this->read_uint(3) + 1;
      this->sample_depth_ = this->read_uint(5) + 1;
      this->num_samples_ = this->read_uint(36);
      this->read_uint(128);
    } else {
      // Variable block
      for (uint32_t i = 0; i < length; i++) {
        this->read_uint(8);

        // Exit early if we run out of data here
        if (this->out_of_data_) {
          return FLAC_DECODER_ERROR_OUT_OF_DATA;
        }
      } // for each byte in block
    }   // variable block
  }     // while not last

  if ((this->sample_rate_ == 0) || (this->num_channels_ == 0) ||
      (this->sample_depth_ == 0) || (this->max_block_size_ == 0)) {
    return FLAC_DECODER_ERROR_BAD_HEADER;
  }
  // this->min_buffer_size_ = this->min_block_size_ * this->num_channels_;

  // Successfully read header
  return FLAC_DECODER_SUCCESS;
} // read_header

FLACDecoderResult FLACDecoder::decode_frame(int16_t *output_buffer,
                                            uint32_t *num_samples, size_t bytes_left) {
  *num_samples = 0;
  this->buffer_index_ = 0;
  this->bytes_left_ = bytes_left;

  if (!this->block_samples_) {
    // freed in free_buffers()
     esphome::ExternalRAMAllocator<int32_t> allocator(esphome::ExternalRAMAllocator<int32_t>::ALLOW_FAILURE);
    this->block_samples_ = allocator.allocate(this->max_block_size_ * this->num_channels_);
        // new int32_t[this->max_block_size_ * this->num_channels_];
  }

  if (this->bytes_left_ == 0) {
    // Done with the stream
    return FLAC_DECODER_NO_MORE_FRAMES;
  }

  // sync code
  if (this->read_uint(14) != 0x3FFE) {
    return FLAC_DECODER_ERROR_SYNC_NOT_FOUND;
  }

  this->read_uint(1);
  this->read_uint(1);

  uint32_t block_size_code = this->read_uint(4);
  uint32_t sample_rate_code = this->read_uint(4);
  uint32_t channel_assignment = this->read_uint(4);

  this->read_uint(3);
  this->read_uint(1);

  uint32_t next_int = this->read_uint(8);
  while (next_int >= 0b11000000) {
    this->read_uint(8);
    next_int = (next_int << 1) & 0xFF;

    if (this->out_of_data_) {
      return FLAC_DECODER_ERROR_OUT_OF_DATA;
    }
  }

  uint32_t block_size = 0;
  if (block_size_code == 1) {
    block_size = 192;
  } else if ((2 <= block_size_code) && (block_size_code <= 5)) {
    block_size = 576 << (block_size_code - 2);
  } else if (block_size_code == 6) {
    block_size = this->read_uint(8) + 1;
  } else if (block_size_code == 7) {
    block_size = this->read_uint(16) + 1;
  } else if (block_size_code <= 15) {
    block_size = 256 << (block_size_code - 8);
  } else {
    return FLAC_DECODER_ERROR_BAD_BLOCK_SIZE_CODE;
  }

  // Assuming that we have sample rate from header
  if (sample_rate_code == 12) {
    this->read_uint(8);
  } else if ((sample_rate_code == 13) || (sample_rate_code == 14)) {
    this->read_uint(16);
  }

  this->read_uint(8);

  // Output buffer size should be max_block_size * num_channels
  this->decode_subframes(block_size, this->sample_depth_, channel_assignment);
  *num_samples = block_size * this->num_channels_;

  // Footer
  this->align_to_byte();
  this->read_uint(16);

  int32_t addend = 0;
  if (this->sample_depth_ == 8) {
    addend = 128;
  }

  // Copy samples to output buffer
  std::size_t output_index = 0;
  for (uint32_t i = 0; i < block_size; i++) {
    for (uint32_t j = 0; j < this->num_channels_; j++) {
      output_buffer[output_index] =
          this->block_samples_[(j * block_size) + i] + addend;
      output_index++;
    }
  }

  return FLAC_DECODER_SUCCESS;
} // decode_frame

void FLACDecoder::free_buffers() {
  if (this->block_samples_) {
    // delete this->block_samples_;
    esphome::ExternalRAMAllocator<int32_t> allocator(esphome::ExternalRAMAllocator<int32_t>::ALLOW_FAILURE);
    allocator.deallocate(this->block_samples_, this->max_block_size_ * this->num_channels_);
    this->block_samples_ = nullptr;
  }

  this->block_result_.clear();
  this->block_result_.shrink_to_fit();
} // free_buffers

std::size_t FLACDecoder::fill_buffer() {
  if (this->bytes_left_ > 0) {
    memmove(this->buffer_, this->buffer_ + this->buffer_index_,
            this->bytes_left_);
  }
  std::size_t bytes_read =
      fread(this->buffer_ + this->bytes_left_, sizeof(uint8_t),
            this->buffer_size_ - this->bytes_left_, stdin);

  this->buffer_index_ = 0;
  this->bytes_left_ += bytes_read;

  return bytes_read;
} // fill_buffer

FLACDecoderResult FLACDecoder::decode_subframes(uint32_t block_size,
                                                uint32_t sample_depth,
                                                uint32_t channel_assignment) {
  FLACDecoderResult result = FLAC_DECODER_SUCCESS;
  if (channel_assignment <= 7) {
    std::size_t block_samples_offset = 0;
    for (std::size_t i = 0; i < channel_assignment + 1; i++) {
      result =
          this->decode_subframe(block_size, sample_depth, block_samples_offset);
      if (result != FLAC_DECODER_SUCCESS) {
        return result;
      }
      block_samples_offset += block_size;
    }
  } else if ((8 <= channel_assignment) && (channel_assignment <= 10)) {
    result = this->decode_subframe(
        block_size, sample_depth + ((channel_assignment == 9) ? 1 : 0), 0);
    if (result != FLAC_DECODER_SUCCESS) {
      return result;
    }
    result = this->decode_subframe(
        block_size, sample_depth + ((channel_assignment == 9) ? 0 : 1),
        block_size);
    if (result != FLAC_DECODER_SUCCESS) {
      return result;
    }

    if (channel_assignment == 8) {
      for (std::size_t i = 0; i < block_size; i++) {
        this->block_samples_[block_size + i] =
            this->block_samples_[i] - this->block_samples_[block_size + i];
      }
    } else if (channel_assignment == 9) {
      for (std::size_t i = 0; i < block_size; i++) {
        this->block_samples_[i] += this->block_samples_[block_size + i];
      }
    } else if (channel_assignment == 10) {
      for (std::size_t i = 0; i < block_size; i++) {
        int32_t side = this->block_samples_[block_size + i];
        int32_t right = this->block_samples_[i] - (side >> 1);
        this->block_samples_[block_size + i] = right;
        this->block_samples_[i] = right + side;
      }
    }
  } else {
    result = FLAC_DECODER_ERROR_RESERVED_CHANNEL_ASSIGNMENT;
  }

  return result;
} // decode_subframes

FLACDecoderResult
FLACDecoder::decode_subframe(uint32_t block_size, uint32_t sample_depth,
                             std::size_t block_samples_offset) {
  this->read_uint(1);

  uint32_t type = this->read_uint(6);
  uint32_t shift = this->read_uint(1);
  if (shift == 1) {
    while (this->read_uint(1) == 0) {
      shift += 1;

      if (this->out_of_data_) {
        return FLAC_DECODER_ERROR_OUT_OF_DATA;
      }
    }
  }

  sample_depth -= shift;

  FLACDecoderResult result = FLAC_DECODER_SUCCESS;
  if (type == 0) {
    // Constant
    int32_t value = this->read_sint(sample_depth) << shift;
    for (std::size_t i = 0; i < block_size; i++) {
      this->block_samples_[block_samples_offset + i] = value;
    }
  } else if (type == 1) {
    // Verbatim
    for (std::size_t i = 0; i < block_size; i++) {
      this->block_samples_[block_samples_offset + i] =
          (this->read_sint(sample_depth) << shift);
    }
  } else if ((8 <= type) && (type <= 12)) {
    // Fixed prediction
    result = this->decode_fixed_subframe(block_size, block_samples_offset,
                                         type - 8, sample_depth);
  } else if ((32 <= type) && (type <= 63)) {
    // LPC (linear predictive coding)
    result = this->decode_lpc_subframe(block_size, block_samples_offset,
                                       type - 31, sample_depth);
    if (result != FLAC_DECODER_SUCCESS) {
      return result;
    }
    if (shift > 0) {
      for (std::size_t i = 0; i < block_size; i++) {
        this->block_samples_[block_samples_offset + i] <<= shift;
      }
    }
  } else {
    result = FLAC_DECODER_ERROR_RESERVED_SUBFRAME_TYPE;
  }

  return result;
} // decode_subframe

FLACDecoderResult
FLACDecoder::decode_fixed_subframe(uint32_t block_size,
                                   std::size_t block_samples_offset,
                                   uint32_t pre_order, uint32_t sample_depth) {
  if (pre_order > 4) {
    return FLAC_DECODER_ERROR_BAD_FIXED_PREDICTION_ORDER;
  }

  FLACDecoderResult result = FLAC_DECODER_SUCCESS;

  this->block_result_.clear();
  for (std::size_t i = 0; i < pre_order; i++) {
    this->block_result_.push_back(this->read_sint(sample_depth));
  }
  result = decode_residuals(block_size);
  if (result != FLAC_DECODER_SUCCESS) {
    return result;
  }
  restore_linear_prediction(FLAC_FIXED_COEFFICIENTS[pre_order], 0);

  std::copy(this->block_result_.begin(), this->block_result_.end(),
            this->block_samples_ + block_samples_offset);

  return result;
} // decode_fixed_subframe

FLACDecoderResult
FLACDecoder::decode_lpc_subframe(uint32_t block_size,
                                 std::size_t block_samples_offset,
                                 uint32_t lpc_order, uint32_t sample_depth) {
  FLACDecoderResult result = FLAC_DECODER_SUCCESS;

  this->block_result_.clear();
  for (std::size_t i = 0; i < lpc_order; i++) {
    this->block_result_.push_back(this->read_sint(sample_depth));
  }

  uint32_t precision = this->read_uint(4) + 1;
  int32_t shift = this->read_sint(5);

  std::vector<int32_t> coefs;
  for (std::size_t i = 0; i < lpc_order; i++) {
    coefs.push_back(this->read_sint(precision));
  }

  result = decode_residuals(block_size);
  if (result != FLAC_DECODER_SUCCESS) {
    return result;
  }
  restore_linear_prediction(coefs, shift);

  std::copy(this->block_result_.begin(), this->block_result_.end(),
            this->block_samples_ + block_samples_offset);

  return result;
} // decode_lpc_subframe

FLACDecoderResult FLACDecoder::decode_residuals(uint32_t block_size) {
  uint32_t method = this->read_uint(2);
  if (method >= 2) {
    return FLAC_DECODER_ERROR_RESERVED_RESIDUAL_CODING_METHOD;
  }

  uint32_t param_bits = 4;
  uint32_t escape_param = 0xF;
  if (method == 1) {
    param_bits = 5;
    escape_param = 0x1F;
  }

  uint32_t partition_order = this->read_uint(4);
  uint32_t num_partitions = 1 << partition_order;
  if ((block_size % num_partitions) != 0) {
    return FLAC_DECODER_ERROR_BLOCK_SIZE_NOT_DIVISIBLE_RICE;
  }

  for (std::size_t i = 0; i < num_partitions; i++) {
    uint32_t count = block_size >> partition_order;
    if (i == 0) {
      count -= this->block_result_.size();
    }
    uint32_t param = this->read_uint(param_bits);
    if (param < escape_param) {
      for (std::size_t j = 0; j < count; j++) {
        this->block_result_.push_back(this->read_rice_sint(param));
      }
    } else {
      std::size_t num_bits = this->read_uint(5);
      for (std::size_t j = 0; j < count; j++) {
        this->block_result_.push_back(this->read_sint(num_bits));
      }
    }
  } // for each partition

  return FLAC_DECODER_SUCCESS;
} // decode_residuals

void FLACDecoder::restore_linear_prediction(const std::vector<int32_t> &coefs,
                                            int32_t shift) {
  for (std::size_t i = coefs.size(); i < this->block_result_.size(); i++) {
    int32_t sum = 0;
    for (std::size_t j = 0; j < coefs.size(); j++) {
      sum += (this->block_result_[i - 1 - j] * coefs[j]);
    }
    this->block_result_[i] += (sum >> shift);
  }
} // restore_linear_prediction

uint32_t FLACDecoder::read_uint(std::size_t num_bits) {
  // if (this->bytes_left_ < this->min_buffer_size_) {
  //   this->fill_buffer();
  // }

  if (this->bytes_left_ == 0) {
    this->out_of_data_ = true;
    return 0;
  }

  while (this->bit_buffer_length_ < num_bits) {
    uint8_t next_byte = this->buffer_[this->buffer_index_];
    this->buffer_index_++;
    this->buffer_total_read_++;
    this->bytes_left_--;
    if (this->bytes_left_ == 0) {
      this->out_of_data_ = true;
      return 0;
    }

    this->bit_buffer_ = (this->bit_buffer_ << 8) | next_byte;
    this->bit_buffer_length_ += 8;
  }

  this->bit_buffer_length_ -= num_bits;
  uint32_t result = this->bit_buffer_ >> this->bit_buffer_length_;
  if (num_bits < 32) {
    result &= FLAC_UINT_MASK[num_bits];
  }

  return result;
} // read_uint

int32_t FLACDecoder::read_sint(std::size_t num_bits) {
  uint32_t next_int = this->read_uint(num_bits);
  return (int32_t)next_int -
         (((int32_t)next_int >> (num_bits - 1)) << num_bits);
} // read_sint

int64_t FLACDecoder::read_rice_sint(uint8_t param) {
  long value = 0;
  while (this->read_uint(1) == 0) {
    value++;
    if (this->out_of_data_) {
      return 0;
    }
  }
  value = (value << param) | this->read_uint(param);
  return (value >> 1) ^ -(value & 1);
} // read_rice_sint

void FLACDecoder::align_to_byte() {
  this->bit_buffer_length_ -= (this->bit_buffer_length_ % 8);
} // align_to_byte

} // namespace flac

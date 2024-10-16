#ifdef USE_ESP_IDF

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "flac_decoder.h"

namespace flac {

FLACDecoderResult FLACDecoder::read_header(size_t buffer_length) {
  this->buffer_index_ = 0;
  this->bytes_left_ = buffer_length;
  this->bit_buffer_ = 0;
  this->bit_buffer_length_ = 0;

  this->out_of_data_ = (buffer_length == 0);

  if (!this->partial_header_read_) {
    // File must start with 'fLaC'
    if (this->read_uint(32) != FLAC_MAGIC_NUMBER) {
      return FLAC_DECODER_ERROR_BAD_MAGIC_NUMBER;
    }
  }

  while (!this->partial_header_last_ || (this->partial_header_length_ > 0)) {
    if (this->bytes_left_ == 0) {
      // We'll try to finish reading it once more data is loaded
      this->partial_header_read_ = true;
      return FLAC_DECODER_HEADER_OUT_OF_DATA;
    }

    if (this->partial_header_length_ == 0) {
      this->partial_header_last_ = this->read_uint(1) != 0;
      this->partial_header_type_ = this->read_uint(7);
      this->partial_header_length_ = this->read_uint(24);
    }

    if (this->partial_header_type_ == 0) {
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

      this->partial_header_length_ = 0;
    } else {
      // Variable block
      while (this->partial_header_length_ > 0) {
        if (this->bytes_left_ == 0) {
          break;
        }
        this->read_uint(8);
        --this->partial_header_length_;
      }
    }  // variable block
  }

  if ((this->sample_rate_ == 0) || (this->num_channels_ == 0) || (this->sample_depth_ == 0) ||
      (this->max_block_size_ == 0)) {
    return FLAC_DECODER_ERROR_BAD_HEADER;
  }

  if ((this->min_block_size_ < 16 ) || (this->min_block_size_ > this->max_block_size_) ||
      (this->max_block_size_ > 65535)) {
    return FLAC_DECODER_ERROR_BAD_HEADER;
  }
  
  // Successfully read header
  return FLAC_DECODER_SUCCESS;
}  // read_header



FLACDecoderResult FLACDecoder::frame_sync_(){
    this->frame_sync_bytes_[0] = 0;
    this->frame_sync_bytes_[1] = 0;
    
    bool second_ff_byte_found = false;
    uint32_t byte;
    
    this->align_to_byte();
    
    while(true){
      if ( second_ff_byte_found ){
        //try if the prev found 0xff is first of the MAGIC NUMBER 
        byte = 0xff;
        second_ff_byte_found = false;
      }
      else{
        byte = this->read_aligned_byte();
      }
      if( byte == 0xff ){
        byte = this->read_aligned_byte();
        if( byte == 0xff ){
          //found a second 0xff, could be the first byte of the MAGIC NUMBER
          second_ff_byte_found = true;
        }
        else if(byte >> 1 == 0x7c) { /* MAGIC NUMBER for the last 6 sync bits and reserved 7th bit */
          this->frame_sync_bytes_[0] = 0xff;
          this->frame_sync_bytes_[1] = byte;
          return FLAC_DECODER_SUCCESS;
        }
      }
      else if (this->out_of_data_){
        return FLAC_DECODER_ERROR_SYNC_NOT_FOUND;
      }
  }
  return FLAC_DECODER_ERROR_SYNC_NOT_FOUND;
}

  
FLACDecoderResult FLACDecoder::decode_frame_header_(){
  uint8_t raw_header[16];
  uint32_t raw_header_len = 0;
  uint32_t new_byte;
  
  if( this->frame_sync_() != FLAC_DECODER_SUCCESS ){
    return FLAC_DECODER_ERROR_SYNC_NOT_FOUND;
  }

  raw_header[raw_header_len++] = this->frame_sync_bytes_[0];
  raw_header[raw_header_len++] = this->frame_sync_bytes_[1];

  /* make sure that reserved bit is 0 */
	if(raw_header[1] & 0x02){ /* MAGIC NUMBER */
		return FLAC_DECODER_ERROR_BAD_MAGIC_NUMBER;
  }
  
  new_byte = this->read_aligned_byte();
  if(new_byte == 0xff) { /* MAGIC NUMBER for the first 8 frame sync bits */
			/* if we get here it means our original sync was erroneous since the sync code cannot appear in the header */
      // needs to search for sync code again
      return FLAC_DECODER_ERROR_SYNC_NOT_FOUND;
  }
  raw_header[raw_header_len++] = new_byte;

  // 9.1.1 Block size bits
  uint8_t block_size_code = raw_header[2] >> 4;
  if (block_size_code == 0) {
    return FLAC_DECODER_ERROR_BAD_BLOCK_SIZE_CODE;
  } else if (block_size_code == 1) {
    this->curr_frame_block_size_ = 192;
  } else if ((2 <= block_size_code) && (block_size_code <= 5)) {
    this->curr_frame_block_size_ = 576 << (block_size_code - 2);
  } else if (block_size_code == 6) {
    // uncommon block size
    // gets parsed later
  } else if (block_size_code == 7) {
    // uncommon block size
    // gets parsed later
  } else if (block_size_code <= 15) {
    this->curr_frame_block_size_ = 256 << (block_size_code - 8);
  } else {
    return FLAC_DECODER_ERROR_BAD_BLOCK_SIZE_CODE;
  }

  // 9.1.2 Sample rate bits
  // Assuming that we have sample rate from header
  // indicates if uncommon sample rate needs to be parsed though
  uint8_t sample_rate_code = raw_header[2] & 0x0f;
  //assert( sample_rate_code == 0 || sample_rate_code == 0b1010 );
  
  // 9.1.3 Channel bits
  new_byte = this->read_aligned_byte();
  if(new_byte == 0xff) { /* MAGIC NUMBER for the first 8 frame sync bits */
			/* if we get here it means our original sync was erroneous since the sync code cannot appear in the header */
      // needs to search for sync code again
      return FLAC_DECODER_ERROR_SYNC_NOT_FOUND;
  }
  raw_header[raw_header_len++] = new_byte;
  this->curr_frame_channel_assign_ = raw_header[3] >> 4;
  
  // 9.1.4 Bit depth bits
  uint8_t bits_per_sample_code = (raw_header[3] & 0x0e) >> 1;
  switch( bits_per_sample_code){
    case 0:
      //take bit depth from streaminfo header
      break;
    case 1: //  8 bit
    case 2: // 12 bit
      // not supported in this version
      return FLAC_DECODER_ERROR_BAD_HEADER;
    case 4: // 16 bit
      break;
    case 5: // 20bit
    case 6: // 24bit
    case 7: // 32bit
    default:
      // not supported in this version
      return FLAC_DECODER_ERROR_BAD_HEADER;
  }

  //reserved bit needs to be zero:
  //ignore raw_header[3] & 0x01 != 0 
  //seems not to be respected by all encoder versions

  
  // 9.1.5. Coded number
  //The coded number is stored in a variable length code like UTF-8 as defined in [RFC3629], but extended to a maximum of 36 bits unencoded, 7 bytes encoded.
  // Interpretation depends on block_size_mode, signalled with (raw_header[1] & 0x01)
  // We don't support file seeking for now so ignore the coded number
  // todo: check for invalid codes, i.e. 0xffffffff (fixed block size) and 0xffffffffffffffff (variable block size)
  uint32_t next_int = this->read_aligned_byte();
  raw_header[raw_header_len++] = next_int;
  while (next_int >= 0b11000000 ) {
    raw_header[raw_header_len++] = this->read_aligned_byte();
    next_int = (next_int << 1) & 0xFF;
  }

  // 9.1.6 Uncommon block size
  if (block_size_code == 6) {
    raw_header[raw_header_len] = this->read_aligned_byte();
    this->curr_frame_block_size_ = raw_header[raw_header_len++] + 1;
  } else if (block_size_code == 7) {
    raw_header[raw_header_len] = this->read_aligned_byte();
    this->curr_frame_block_size_ =  raw_header[raw_header_len++] << 8;
    raw_header[raw_header_len] = this->read_aligned_byte();
    this->curr_frame_block_size_ |= raw_header[raw_header_len++];
    this->curr_frame_block_size_ += 1;
  }
  
  // 9.1.7 Uncommon sample rate 
  // Assuming that we have sample rate from header
  if (sample_rate_code == 12) {
    raw_header[raw_header_len++] = this->read_aligned_byte();
  } else if ((sample_rate_code == 13) || (sample_rate_code == 14)) {
    raw_header[raw_header_len++] = this->read_aligned_byte();
    raw_header[raw_header_len++] = this->read_aligned_byte();
  }

  // out of data wasn't checked after each read, check it now
  if(this->out_of_data_){
    return FLAC_DECODER_ERROR_OUT_OF_DATA;
  }

  // 9.1.8 Frame header CRC
  uint8_t crc_read = this->read_aligned_byte();
    
  return FLAC_DECODER_SUCCESS;
}

FLACDecoderResult FLACDecoder::decode_frame(size_t buffer_length, int16_t *output_buffer, uint32_t *num_samples) {
  this->buffer_index_ = 0;
  this->bytes_left_ = buffer_length;
  this->out_of_data_ = false;
  
  FLACDecoderResult ret = FLAC_DECODER_SUCCESS;
  
  *num_samples = 0;

  if (!this->block_samples_) {
    // freed in free_buffers()
    esphome::ExternalRAMAllocator<int32_t> allocator(esphome::ExternalRAMAllocator<int32_t>::ALLOW_FAILURE);
    this->block_samples_ = allocator.allocate(this->max_block_size_ * this->num_channels_);
  }
  if (!this->block_samples_) {
    return FLAC_DECODER_ERROR_MEMORY_ALLOCATION_ERROR;
  }
  
  if (this->bytes_left_ == 0) {
    // buffer is empty when called
    return FLAC_DECODER_NO_MORE_FRAMES;
  }
  
  uint64_t previous_bit_buffer = this->bit_buffer_;
  uint32_t previous_bit_buffer_length = this->bit_buffer_length_;
  ret = this->decode_frame_header_();
  if( ret != FLAC_DECODER_SUCCESS ){
    return ret;
  }

  // Memory is allocated based on the maximum block size. 
  // Ensure that no out-of-bounds access occurs, particularly in case of parsing errors.
  if( this->curr_frame_block_size_ > this->max_block_size_ ){
    return FLAC_DECODER_ERROR_BLOCK_SIZE_OUT_OF_RANGE;
  }

  // Output buffer size (in sample) should be max_block_size * num_channels
  this->decode_subframes(this->curr_frame_block_size_, this->sample_depth_, this->curr_frame_channel_assign_);
  *num_samples = this->curr_frame_block_size_ * this->num_channels_;

  if (this->bytes_left_ < 2) {
    this->bit_buffer_ = previous_bit_buffer;
    this->bit_buffer_length_ = previous_bit_buffer_length;
    return FLAC_DECODER_ERROR_OUT_OF_DATA;
  }
  
  // Footer
  this->align_to_byte();
  this->read_uint(16);

  int32_t addend = 0;
  if (this->sample_depth_ == 8) {
    addend = 128;
  }

  // Copy samples to output buffer
  std::size_t output_index = 0;
  for (uint32_t i = 0; i < this->curr_frame_block_size_; i++) {
    for (uint32_t j = 0; j < this->num_channels_; j++) {
      output_buffer[output_index] = this->block_samples_[(j * this->curr_frame_block_size_) + i] + addend;
      output_index++;
    }
  }

  return FLAC_DECODER_SUCCESS;
}  // decode_frame

void FLACDecoder::free_buffers() {
  if (this->block_samples_) {
    // delete this->block_samples_;
    esphome::ExternalRAMAllocator<int32_t> allocator(esphome::ExternalRAMAllocator<int32_t>::ALLOW_FAILURE);
    allocator.deallocate(this->block_samples_, this->max_block_size_ * this->num_channels_);
    this->block_samples_ = nullptr;
  }
}  // free_buffers

FLACDecoderResult FLACDecoder::decode_subframes(uint32_t block_size, uint32_t sample_depth,
                                                uint32_t channel_assignment) {
  FLACDecoderResult result = FLAC_DECODER_SUCCESS;
  if (channel_assignment <= 7) {
    std::size_t block_samples_offset = 0;
    for (std::size_t i = 0; i < channel_assignment + 1; i++) {
      result = this->decode_subframe(block_size, sample_depth, block_samples_offset);
      if (result != FLAC_DECODER_SUCCESS) {
        return result;
      }
      block_samples_offset += block_size;
    }
  } else if ((8 <= channel_assignment) && (channel_assignment <= 10)) {
    result = this->decode_subframe(block_size, sample_depth + ((channel_assignment == 9) ? 1 : 0), 0);
    if (result != FLAC_DECODER_SUCCESS) {
      return result;
    }
    result = this->decode_subframe(block_size, sample_depth + ((channel_assignment == 9) ? 0 : 1), block_size);
    if (result != FLAC_DECODER_SUCCESS) {
      return result;
    }

    if (channel_assignment == 8) {
      for (std::size_t i = 0; i < block_size; i++) {
        this->block_samples_[block_size + i] = this->block_samples_[i] - this->block_samples_[block_size + i];
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
}  // decode_subframes

FLACDecoderResult FLACDecoder::decode_subframe(uint32_t block_size, uint32_t sample_depth,
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
    std::fill(this->block_samples_ + block_samples_offset, this->block_samples_ + block_samples_offset + block_size, value );
  } else if (type == 1) {
    // Verbatim
    for (std::size_t i = 0; i < block_size; i++) {
      this->block_samples_[block_samples_offset + i] = (this->read_sint(sample_depth) << shift);
    }
  } else if ((8 <= type) && (type <= 12)) {
    // Fixed prediction
    result = this->decode_fixed_subframe(block_size, block_samples_offset, type - 8, sample_depth);
  } else if ((32 <= type) && (type <= 63)) {
    // LPC (linear predictive coding)
    result = this->decode_lpc_subframe(block_size, block_samples_offset, type - 31, sample_depth);
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
}  // decode_subframe

FLACDecoderResult FLACDecoder::decode_fixed_subframe(uint32_t block_size, std::size_t block_samples_offset,
                                                     uint32_t pre_order, uint32_t sample_depth) {
  if (pre_order > 4) {
    return FLAC_DECODER_ERROR_BAD_FIXED_PREDICTION_ORDER;
  }

  FLACDecoderResult result = FLAC_DECODER_SUCCESS;

  int32_t* const sub_frame_buffer = this->block_samples_ + block_samples_offset;
  int32_t *out_ptr = sub_frame_buffer;
  
  //warum-up samples
  for (std::size_t i = 0; i < pre_order; i++) {
    *(out_ptr++) = this->read_sint(sample_depth);
  }
  result = decode_residuals(sub_frame_buffer, pre_order, block_size);
  if (result != FLAC_DECODER_SUCCESS) {
    return result;
  }
  restore_linear_prediction(sub_frame_buffer, block_size, FLAC_FIXED_COEFFICIENTS[pre_order], 0);
  return result;

}  // decode_fixed_subframe

FLACDecoderResult FLACDecoder::decode_lpc_subframe(uint32_t block_size, std::size_t block_samples_offset,
                                                   uint32_t lpc_order, uint32_t sample_depth) {
  FLACDecoderResult result = FLAC_DECODER_SUCCESS;

  int32_t* const sub_frame_buffer = this->block_samples_ + block_samples_offset;
  int32_t* out_ptr = sub_frame_buffer;
  
  for (std::size_t i = 0; i < lpc_order; i++) {
    *(out_ptr++) = this->read_sint(sample_depth);
  }

  uint32_t precision = this->read_uint(4) + 1;
  int32_t shift = this->read_sint(5);

  std::vector<int16_t> coefs;
  coefs.resize(lpc_order + 1);
  for (std::size_t i = 0; i < lpc_order; i++) {
    coefs[lpc_order - i - 1] = this->read_sint(precision);
  }
  coefs[lpc_order] = 1 << shift;

  result = decode_residuals(sub_frame_buffer, lpc_order, block_size);
  if (result != FLAC_DECODER_SUCCESS) {
    return result;
  }
  restore_linear_prediction(sub_frame_buffer, block_size, coefs, shift);

  return result;
}  // decode_lpc_subframe 

FLACDecoderResult FLACDecoder::decode_residuals(int32_t* sub_frame_buffer, size_t warm_up_samples, uint32_t block_size) {
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

  int32_t *out_ptr = sub_frame_buffer + warm_up_samples;
  {
    uint32_t count = (block_size >> partition_order) - warm_up_samples;
    uint32_t param = this->read_uint(param_bits);
    if (param < escape_param) {
      for (std::size_t j = 0; j < count; j++) {
        *(out_ptr++) = this->read_rice_sint(param);
      }
    } else {
      std::size_t num_bits = this->read_uint(5);
      if( num_bits == 0 ){
        std::memset( out_ptr, 0, count * sizeof(int32_t));
        out_ptr += count;
      } else {
        for (std::size_t j = 0; j < count; j++) {
          *(out_ptr++) = this->read_sint(num_bits);
        }
      }
    }
  }

  uint32_t count = block_size >> partition_order;
  for (std::size_t i = 1; i < num_partitions; i++) {
    uint32_t param = this->read_uint(param_bits);
    if (param < escape_param) {
      for (std::size_t j = 0; j < count; j++) {
        *(out_ptr++) = this->read_rice_sint(param);
      }
    } else {
      std::size_t num_bits = this->read_uint(5);
      if( num_bits == 0 ){
        std::memset( out_ptr, 0, count * sizeof(int32_t));
        out_ptr += count;
      } else {
        for (std::size_t j = 0; j < count; j++) {
          *(out_ptr++) = this->read_sint(num_bits);
        }
      }
    }
  }  // for each partition

  return FLAC_DECODER_SUCCESS;
}  // decode_residuals

void FLACDecoder::restore_linear_prediction(int32_t* sub_frame_buffer, size_t num_of_samples, const std::vector<int16_t> &coefs, int32_t shift) {
  
  for (std::size_t i = 0; i < num_of_samples - coefs.size() + 1; i++) {
    int32_t sum = 0;
    for (std::size_t j = 0; j < coefs.size(); ++j) {
      sum += (sub_frame_buffer[i + j] * coefs[j]);
    }
    sub_frame_buffer[i + coefs.size() - 1] = (sum >> shift);
  }
}  // restore_linear_prediction

uint32_t FLACDecoder::read_aligned_byte(){
  //assumes byte alignment
  assert( this->bit_buffer_length_ % 8 == 0 );
  
  if( this->bit_buffer_length_ >= 8 ){
    this->bit_buffer_length_ -=8;
    uint32_t ret_byte = this->bit_buffer_ >> this->bit_buffer_length_; 
    return ret_byte & FLAC_UINT_MASK[8];
  }
  
  if (this->bytes_left_ == 0) {
    this->out_of_data_ = true;
    return 0;
  }
  
  uint8_t next_byte = this->buffer_[this->buffer_index_];
  this->buffer_index_++;
  this->bytes_left_--;
  
  return next_byte;

}

uint32_t FLACDecoder::read_uint(std::size_t num_bits) {
  while (this->bit_buffer_length_ < num_bits) {
    if (this->bytes_left_ == 0) {
      this->out_of_data_ = true;
      return 0;
    }
    uint8_t next_byte = this->buffer_[this->buffer_index_];
    this->buffer_index_++;
    this->bytes_left_--;

    this->bit_buffer_ = (this->bit_buffer_ << 8) | next_byte;
    this->bit_buffer_length_ += 8;
  }

  this->bit_buffer_length_ -= num_bits;
  uint32_t result = this->bit_buffer_ >> this->bit_buffer_length_;
  if (num_bits < 32) {
    result &= FLAC_UINT_MASK[num_bits];
  }

  return result;
}  // read_uint

int32_t FLACDecoder::read_sint(std::size_t num_bits) {
  uint32_t next_int = this->read_uint(num_bits);
  return (int32_t) next_int - (((int32_t) next_int >> (num_bits - 1)) << num_bits);
}  // read_sint

// why int64_t? standard restricts residuals to fit into 32bit
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
}  // read_rice_sint

void FLACDecoder::align_to_byte() { this->bit_buffer_length_ -= (this->bit_buffer_length_ % 8); }  // align_to_byte

}  // namespace flac
#endif
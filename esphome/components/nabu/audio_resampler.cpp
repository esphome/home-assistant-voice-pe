#ifdef USE_ESP_IDF

#include "audio_resampler.h"

#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace nabu {

static const size_t NUM_TAPS = 32;
static const size_t NUM_FILTERS = 32;
static const bool USE_PRE_POST_FILTER = true;

AudioResampler::AudioResampler(RingBuffer *input_ring_buffer, RingBuffer *output_ring_buffer,
                               size_t internal_buffer_samples) {
  this->input_ring_buffer_ = input_ring_buffer;
  this->output_ring_buffer_ = output_ring_buffer;
  this->internal_buffer_samples_ = internal_buffer_samples;

  ExternalRAMAllocator<int16_t> int16_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  this->input_buffer_ = int16_allocator.allocate(internal_buffer_samples);
  this->output_buffer_ = int16_allocator.allocate(internal_buffer_samples);

  ExternalRAMAllocator<float> float_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);
  this->float_input_buffer_ = float_allocator.allocate(internal_buffer_samples);
  this->float_output_buffer_ = float_allocator.allocate(internal_buffer_samples);
}

AudioResampler::~AudioResampler() {
  ExternalRAMAllocator<int16_t> int16_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  ExternalRAMAllocator<float> float_allocator(ExternalRAMAllocator<float>::ALLOW_FAILURE);

  if (this->input_buffer_ != nullptr) {
    int16_allocator.deallocate(this->input_buffer_, this->internal_buffer_samples_);
  }
  if (this->output_buffer_ != nullptr) {
    int16_allocator.deallocate(this->output_buffer_, this->internal_buffer_samples_);
  }
  if (this->float_input_buffer_ != nullptr) {
    float_allocator.deallocate(this->float_input_buffer_, this->internal_buffer_samples_);
  }
  if (this->float_output_buffer_ != nullptr) {
    float_allocator.deallocate(this->float_output_buffer_, this->internal_buffer_samples_);
  }
  if (this->resampler_ != nullptr) {
    resampleFree(this->resampler_);
    this->resampler_ = nullptr;
  }
}

bool AudioResampler::start(media_player::StreamInfo &stream_info, uint32_t target_sample_rate) {
  this->stream_info_ = stream_info;

  this->input_buffer_current_ = this->input_buffer_;
  this->input_buffer_length_ = 0;
  this->float_input_buffer_current_ = this->float_input_buffer_;
  this->float_input_buffer_length_ = 0;

  this->output_buffer_current_ = this->output_buffer_;
  this->output_buffer_length_ = 0;
  this->float_output_buffer_current_ = this->float_output_buffer_;
  this->float_output_buffer_length_ = 0;

  this->needs_mono_to_stereo_ = (stream_info.channels != 2);

  if ((stream_info.channels > 2) || (stream_info_.bits_per_sample != 16)) {
    // TODO: Make these values configurable
    return false;
  }

  if (stream_info.channels > 0) {
    this->channel_factor_ = 2 / stream_info.channels;
    printf("Converting %d channels to 2 channels\n", stream_info.channels);
  }

  if (stream_info.sample_rate != target_sample_rate) {
    // if (stream_info.sample_rate == 48000) {
    //   // Special case, we can do this a lot faster with esp-dsp code!
    //   const uint8_t decimation = 48000 / 16000;
    //   const float fir_out_offset = 0;  //((FIR_FILTER_LENGTH / decimation / 2) - 1);

    //   int8_t shift = this->generate_q15_fir_coefficients_(this->fir_filter_coeffecients_, (uint32_t) FIR_FILTER_LENGTH,
    //                                                       (float) 0.5 / decimation);
    //   // dsps_16_array_rev(this->fir_filter_coeffecients_, (uint32_t) FIR_FILTER_LENGTH);
    //   dsps_fird_init_s16(&this->fir_filter_, this->fir_filter_coeffecients_, this->fir_delay_, FIR_FILTER_LENGTH,
    //                      decimation, fir_out_offset, -shift);
    //   this->decimation_filter_ = true;
    //   this->needs_resampling_ = true;
    //   // memset(this->fir_delay_, 0, FIR_FILTER_LENGTH*sizeof(int16_t));
    // } else 
    {
      int flags = 0;

      this->needs_resampling_ = true;

      this->sample_ratio_ = static_cast<float>(target_sample_rate) / static_cast<float>(stream_info.sample_rate);

      printf("Resampling from %d Hz to %d Hz\n", stream_info.sample_rate, target_sample_rate);

      if (this->sample_ratio_ < 1.0) {
        this->lowpass_ratio_ -= (10.24 / 16);

        if (this->lowpass_ratio_ < 0.84) {
          this->lowpass_ratio_ = 0.84;
        }

        if (this->lowpass_ratio_ < this->sample_ratio_) {
          // avoid discontinuities near unity sample ratios
          this->lowpass_ratio_ = this->sample_ratio_;
        }
      }
      if (this->lowpass_ratio_ * this->sample_ratio_ < 0.98 && USE_PRE_POST_FILTER) {
        float cutoff = this->lowpass_ratio_ * this->sample_ratio_ / 2.0;
        biquad_lowpass(&this->lowpass_coeff_, cutoff);
        this->pre_filter_ = true;
      }

      if (this->lowpass_ratio_ / this->sample_ratio_ < 0.98 && USE_PRE_POST_FILTER && !this->pre_filter_) {
        float cutoff = this->lowpass_ratio_ / this->sample_ratio_ / 2.0;
        biquad_lowpass(&this->lowpass_coeff_, cutoff);
        this->post_filter_ = true;
      }

      if (this->pre_filter_ || this->post_filter_) {
        for (int i = 0; i < stream_info.channels; ++i) {
          biquad_init(&this->lowpass_[i][0], &this->lowpass_coeff_, 1.0);
          biquad_init(&this->lowpass_[i][1], &this->lowpass_coeff_, 1.0);
        }
      }

      if (this->sample_ratio_ < 1.0) {
        this->resampler_ = resampleInit(stream_info.channels, NUM_TAPS, NUM_FILTERS,
                                        this->sample_ratio_ * this->lowpass_ratio_, flags | INCLUDE_LOWPASS);
      } else if (this->lowpass_ratio_ < 1.0) {
        this->resampler_ =
            resampleInit(stream_info.channels, NUM_TAPS, NUM_FILTERS, this->lowpass_ratio_, flags | INCLUDE_LOWPASS);
      } else {
        this->resampler_ = resampleInit(stream_info.channels, NUM_TAPS, NUM_FILTERS, 1.0, flags);
      }

      resampleAdvancePosition(this->resampler_, NUM_TAPS / 2.0);
    }
  } else {
    this->needs_resampling_ = false;
  }

  return true;
}

AudioResamplerState AudioResampler::resample(bool stop_gracefully) {
  if (stop_gracefully) {
    if ((this->input_ring_buffer_->available() == 0) && (this->output_ring_buffer_->available() == 0) &&
        (this->input_buffer_length_ == 0) && (this->output_buffer_length_ == 0)) {
      return AudioResamplerState::FINISHED;
    }
  }

  if (this->output_buffer_length_ > 0) {
    size_t bytes_free = this->output_ring_buffer_->free();
    size_t bytes_to_write = std::min(this->output_buffer_length_, bytes_free);

    if (bytes_to_write > 0) {
      size_t bytes_written = this->output_ring_buffer_->write((void *) this->output_buffer_current_, bytes_to_write);

      this->output_buffer_current_ += bytes_written / sizeof(int16_t);
      this->output_buffer_length_ -= bytes_written;
    }

    return AudioResamplerState::RESAMPLING;
  }

  //////
  // Refill input buffer
  //////

  // Depending on if we are converting mono to stereo or if we are upsampling, we may need to restrict how many input samples to load
  // Mono to stereo -> cut in half
  // Upsampling -> reduce by a factor of the ceiling of sample_ratio_

  size_t max_input_samples = this->internal_buffer_samples_;

  max_input_samples /= this->stream_info_.channels;
  
  uint32_t upsampling_factor = std::ceil(this->sample_ratio_);
  max_input_samples /= upsampling_factor;

  // Move old data to the start of the buffer
  if (this->input_buffer_length_ > 0) {
    memmove((void *) this->input_buffer_, (void *) this->input_buffer_current_, this->input_buffer_length_);
  }
  this->input_buffer_current_ = this->input_buffer_;

  // Copy new data to the end of the of the buffer
  size_t bytes_available = this->input_ring_buffer_->available();
  size_t bytes_to_read =
      std::min(bytes_available, max_input_samples * sizeof(int16_t) - this->input_buffer_length_);

  if (bytes_to_read > 0) {
    int16_t *new_input_buffer_data = this->input_buffer_ + this->input_buffer_length_ / sizeof(int16_t);
    size_t bytes_read = this->input_ring_buffer_->read((void *) new_input_buffer_data, bytes_to_read);

    this->input_buffer_length_ += bytes_read;
  }

  if (this->needs_resampling_) {
    if (this->decimation_filter_) {
      if (this->needs_mono_to_stereo_) {
        if (this->input_buffer_length_ > 0) {
          size_t available_samples = this->input_buffer_length_ / sizeof(int16_t);

          if (available_samples / 3 == 0) {
            this->input_buffer_current_ = this->input_buffer_;
            this->input_buffer_length_ = 0;
          } else {
            dsps_fird_s16_aes3(&this->fir_filter_, this->input_buffer_current_, this->output_buffer_,
                               available_samples / 3);

            size_t output_samples = available_samples / 3;

            this->input_buffer_current_ += output_samples * 3;
            this->input_buffer_length_ -= output_samples * 3 * sizeof(int16_t);

            this->output_buffer_current_ = this->output_buffer_;
            this->output_buffer_length_ += output_samples * sizeof(int16_t);
          }
        }
      } else {
        // Interleaved stereo samples
        // TODO: This doesn't sound correct! I need to use separate filters for each channel so the delay line isn't mixed
        size_t available_samples = this->input_buffer_length_ / sizeof(int16_t);
        for (int i = 0; i < available_samples / 2; ++i) {
          // split interleaved samples into two separate streams
          this->output_buffer_[i] = this->input_buffer_[2 * i];
          this->output_buffer_[i + available_samples / 2] = this->input_buffer_[2 * i + 1];
        }
        std::memcpy(this->input_buffer_, this->output_buffer_, available_samples * sizeof(int16_t));
        dsps_fird_s16_aes3(&this->fir_filter_, this->input_buffer_, this->output_buffer_, (available_samples / 3) / 2);
        dsps_fird_s16_aes3(&this->fir_filter_, this->input_buffer_ + available_samples / 2,
                           this->output_buffer_ + (available_samples / 3) / 2, (available_samples / 3) / 2);
        std::memcpy(this->input_buffer_, this->output_buffer_, available_samples * sizeof(int16_t));
        for (int i = 0; i < available_samples / 2; ++i) {
          this->output_buffer_[2 * i] = this->input_buffer_[i];
          this->output_buffer_[2 * i + 1] = this->input_buffer_[available_samples / 2 + i];
        }

        size_t output_samples = available_samples / 3;

        this->input_buffer_current_ += output_samples * 3;
        this->input_buffer_length_ -= output_samples * 3 * sizeof(int16_t);

        this->output_buffer_current_ = this->output_buffer_;
        this->output_buffer_length_ += output_samples * sizeof(int16_t);
      }
    } else {
      if (this->input_buffer_length_ > 0) {
        // Samples are indiviudal int16 values. Frames include 1 sample for mono and 2 samples for stereo
        // Be careful converting between bytes, samples, and frames!
        // 1 sample = 2 bytes = sizeof(int16_t)
        // if mono:
        //    1 frame = 1 sample
        // if stereo:
        //    1 frame = 2 samples (left and right)

        size_t samples_read = this->input_buffer_length_ / sizeof(int16_t);

        for (int i = 0; i < samples_read; ++i) {
          this->float_input_buffer_[i] = static_cast<float>(this->input_buffer_[i]) / 32768.0f;
        }

        size_t frames_read = samples_read / this->stream_info_.channels;

        if (this->pre_filter_) {
          for (int i = 0; i < this->stream_info_.channels; ++i) {
            biquad_apply_buffer(&this->lowpass_[i][0], this->float_input_buffer_ + i, frames_read,
                                this->stream_info_.channels);
            biquad_apply_buffer(&this->lowpass_[i][1], this->float_input_buffer_ + i, frames_read,
                                this->stream_info_.channels);
          }
        }

        ResampleResult res;

        res = resampleProcessInterleaved(this->resampler_, this->float_input_buffer_, frames_read,
                                         this->float_output_buffer_,
                                         this->internal_buffer_samples_ / this->channel_factor_, this->sample_ratio_);

        size_t frames_used = res.input_used;
        size_t samples_used = frames_used * this->stream_info_.channels;

        size_t frames_generated = res.output_generated;
        if (this->post_filter_) {
          for (int i = 0; i < this->stream_info_.channels; ++i) {
            biquad_apply_buffer(&this->lowpass_[i][0], this->float_output_buffer_ + i, frames_generated,
                                this->stream_info_.channels);
            biquad_apply_buffer(&this->lowpass_[i][1], this->float_output_buffer_ + i, frames_generated,
                                this->stream_info_.channels);
          }
        }

        size_t samples_generated = frames_generated * this->stream_info_.channels;

        for (int i = 0; i < samples_generated; ++i) {
          this->output_buffer_[i] = static_cast<int16_t>(this->float_output_buffer_[i] * 32767);
        }

        this->input_buffer_current_ += samples_used;
        this->input_buffer_length_ -= samples_used * sizeof(int16_t);

        this->output_buffer_current_ = this->output_buffer_;
        this->output_buffer_length_ += samples_generated * sizeof(int16_t);
      }
    }
  } else {
    size_t bytes_to_transfer =
        std::min(this->internal_buffer_samples_ / this->channel_factor_ * sizeof(int16_t), this->input_buffer_length_);
    std::memcpy((void *) this->output_buffer_, (void *) this->input_buffer_current_, bytes_to_transfer);

    this->input_buffer_current_ += bytes_to_transfer / sizeof(int16_t);
    this->input_buffer_length_ -= bytes_to_transfer;

    this->output_buffer_current_ = this->output_buffer_;
    this->output_buffer_length_ += bytes_to_transfer;
  }

  if (this->needs_mono_to_stereo_) {
    // Convert mono to stereo
    for (int i = this->output_buffer_length_ / (sizeof(int16_t)) - 1; i >= 0; --i) {
      this->output_buffer_[2 * i] = this->output_buffer_[i];
      this->output_buffer_[2 * i + 1] = this->output_buffer_[i];
    }

    this->output_buffer_length_ *= 2;  // double the bytes for stereo samples
  }
  return AudioResamplerState::RESAMPLING;
}

int16_t AudioResampler::float_to_q15_(float q, uint32_t shift) { return (int16_t) (q * pow(2, 15 + shift)); }

int8_t AudioResampler::generate_q15_fir_coefficients_(int16_t *fir_coeffs, const unsigned int fir_len, const float ft) {
  // Even or odd length of the FIR filter
  const bool is_odd = (fir_len % 2) ? (true) : (false);
  const float fir_order = (float) (fir_len - 1);

  // Window coefficients
  float *fir_window = (float *) malloc(fir_len * sizeof(float));
  dsps_wind_blackman_harris_f32(fir_window, fir_len);

  float *float_coeffs = (float *) malloc(fir_len * sizeof(float));

  float max_coeff = 0.0;
  float min_coeff = 0.0;
  for (int i = 0; i < fir_len; i++) {
    if ((i == fir_order / 2) && (is_odd)) {
      float_coeffs[i] = 2 * ft;
    } else {
      float_coeffs[i] = sinf((2 * M_PI * ft * (i - fir_order / 2))) / (M_PI * (i - fir_order / 2));
    }

    float_coeffs[i] *= fir_window[i];
    if (float_coeffs[i] > max_coeff) {
      max_coeff = float_coeffs[i];
    }
    if (float_coeffs[i] < min_coeff) {
      min_coeff = float_coeffs[i];
    }
  }

  float max_abs_coeffs = fmaxf(fabsf(min_coeff), max_coeff);

  int32_t shift = 0;
  for (int i = 1; i < 15; ++i) {
    if (max_abs_coeffs < pow(2, -i)) {
      ++shift;
    }
  }

  for (int i = 0; i < fir_len; ++i) {
    fir_coeffs[i] = float_to_q15_(float_coeffs[i], shift);
  }

  free(fir_window);

  return shift;
}

}  // namespace nabu
}  // namespace esphome

#endif
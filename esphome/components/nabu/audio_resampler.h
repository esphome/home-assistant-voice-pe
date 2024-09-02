#pragma once

#ifdef USE_ESP_IDF

#include "biquad.h"
#include "resampler.h"

#include "esp_dsp.h"

#include "esphome/components/media_player/media_player.h"
#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace nabu {

static const uint32_t FIR_FILTER_LENGTH = 16;

enum class AudioResamplerState : uint8_t {
  INITIALIZED = 0,
  RESAMPLING,
  FINISHED,
  FAILED,
};

struct ResampleInfo {
  bool resample;
  bool mono_to_stereo;
};

class ESPIntegerUpsampler;

class AudioResampler {
 public:
  AudioResampler(esphome::RingBuffer *input_ring_buffer, esphome::RingBuffer *output_ring_buffer,
                 size_t internal_buffer_samples);
  ~AudioResampler();

  /// @brief Sets up the various bits necessary to resample
  /// @param stream_info the incoming sample rate, bits per sample, and number of channels
  /// @param target_sample_rate the necessary sample rate to convert to
  /// @return ESP_OK if it is able to convert the incoming stream or an error otherwise
  esp_err_t start(media_player::StreamInfo &stream_info, uint32_t target_sample_rate, ResampleInfo &resample_info);

  AudioResamplerState resample(bool stop_gracefully);

 protected:
  esp_err_t allocate_buffers_();

  esphome::RingBuffer *input_ring_buffer_;
  esphome::RingBuffer *output_ring_buffer_;
  size_t internal_buffer_samples_;

  int16_t *input_buffer_{nullptr};
  int16_t *input_buffer_current_{nullptr};
  size_t input_buffer_length_;

  int16_t *output_buffer_{nullptr};
  int16_t *output_buffer_current_{nullptr};
  size_t output_buffer_length_;

  float *float_input_buffer_{nullptr};
  float *float_input_buffer_current_{nullptr};
  size_t float_input_buffer_length_;

  float *float_output_buffer_{nullptr};
  float *float_output_buffer_current_{nullptr};
  size_t float_output_buffer_length_;

  media_player::StreamInfo stream_info_;
  ResampleInfo resample_info_;

  Resample *resampler_{nullptr};

  Biquad lowpass_[2][2];
  BiquadCoefficients lowpass_coeff_;

  bool use_effecient_upsampler_{false};
  ESPIntegerUpsampler *effecient_upsampler_;

  float sample_ratio_{1.0};
  float lowpass_ratio_{1.0};
  uint8_t channel_factor_{1};

  bool pre_filter_{false};
  bool post_filter_{false};
};

class ESPIntegerUpsampler {
 public:
  ESPIntegerUpsampler(uint8_t integer_upsample_factor) {
    this->integer_upsample_factor_ = integer_upsample_factor;

    float ft_cutoff = 0.333333f;
    int8_t shift =
        this->generate_q15_fir_coefficients_(this->fir_filter_coeffecients_, (uint32_t) FIR_FILTER_LENGTH, ft_cutoff);
    // dsps_16_array_rev(this->fir_filter_coeffecients_, (uint32_t) FIR_FILTER_LENGTH);
    dsps_fird_init_s16(&this->fir_filter_, this->fir_filter_coeffecients_, NULL, FIR_FILTER_LENGTH, 1, 0, -shift);
  }
  ~ESPIntegerUpsampler() { dsps_fird_s16_aexx_free(&this->fir_filter_); }

  uint8_t get_integer_upsample_factor() { return this->integer_upsample_factor_; }

  size_t upsample(int16_t *input_buffer, int16_t *output_buffer, size_t input_samples) {
    size_t output_samples = input_samples * this->integer_upsample_factor_;

    this->temporary_buffer_.clear();
    this->temporary_buffer_.reserve(output_samples);

    // Insert zeros between real samples
    for (int i = 0; i < input_samples; ++i) {
      this->temporary_buffer_.push_back(input_buffer[i]);
      for (int j = 0; j < this->integer_upsample_factor_ - 1; ++j) {
        this->temporary_buffer_.push_back(0);
      }
    }

    // Pass through low-pass filter to smooth the signal
    dsps_fird_s16_ansi(&this->fir_filter_, this->temporary_buffer_.data(), output_buffer, output_samples);
    // dsps_fird_s16_aes3(&this->fir_filter_, this->temporary_buffer_.data(), output_buffer, output_samples);

    return output_samples;
  }

 protected:
  fir_s16_t fir_filter_;

  std::vector<int16_t, esphome::ExternalRAMAllocator<int16_t>> temporary_buffer_;
  uint8_t integer_upsample_factor_{false};
  int16_t fir_filter_coeffecients_[FIR_FILTER_LENGTH];
  // int16_t fir_delay_[FIR_FILTER_LENGTH];
  // alignas(16) int16_t fir_filter_coeffecients_[FIR_FILTER_LENGTH];
  // alignas(16) int16_t fir_delay_[FIR_FILTER_LENGTH];
  int16_t float_to_q15_(float q, uint32_t shift);
  int8_t generate_q15_fir_coefficients_(int16_t *fir_coeffs, const unsigned int fir_len, const float ft);
};
}  // namespace nabu
}  // namespace esphome

#endif
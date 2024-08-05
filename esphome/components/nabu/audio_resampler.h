#pragma once

#ifdef USE_ESP_IDF

#include "audio_pipeline.h"

#include "biquad.h"
#include "resampler.h"

#include "esp_dsp.h"

#include "esphome/components/media_player/media_player.h"
#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace nabu {

static const uint32_t FIR_FILTER_LENGTH = 96;

enum class AudioResamplerState : uint8_t {
  INITIALIZED = 0,
  RESAMPLING,
  FINISHED,
  FAILED,
};

class AudioResampler {
 public:
  AudioResampler(esphome::RingBuffer *input_ring_buffer, esphome::RingBuffer *output_ring_buffer,
                 size_t internal_buffer_samples);
  ~AudioResampler();

  /// @brief Sets up the various bits necessary to resample
  /// @param stream_info the incoming sample rate, bits per sample, and number of channels
  /// @param target_sample_rate the necessary sample rate to convert to
  /// @return True if it convert the incoming stream, false otherwise
  bool start(media_player::StreamInfo &stream_info, uint32_t target_sample_rate);

  AudioResamplerState resample(bool stop_gracefully);

 protected:
  esphome::RingBuffer *input_ring_buffer_;
  esphome::RingBuffer *output_ring_buffer_;
  size_t internal_buffer_samples_;

  int16_t *input_buffer_;
  int16_t *input_buffer_current_;
  size_t input_buffer_length_;

  int16_t *output_buffer_;
  int16_t *output_buffer_current_;
  size_t output_buffer_length_;

  float *float_input_buffer_;
  float *float_input_buffer_current_;
  size_t float_input_buffer_length_;

  float *float_output_buffer_;
  float *float_output_buffer_current_;
  size_t float_output_buffer_length_;

  media_player::StreamInfo stream_info_;
  bool needs_resampling_{false};
  bool needs_mono_to_stereo_{false};

  Resample *resampler_{nullptr};

  Biquad lowpass_[2][2];
  BiquadCoefficients lowpass_coeff_;

  float sample_ratio_{1.0};
  float lowpass_ratio_{1.0};
  uint8_t channel_factor_{1};

  bool pre_filter_{false};
  bool post_filter_{false};

  // The following is used to create faster decimation filter when we resample from 48 kHz to 16 kHz
  // TODO: There seems to be some aliasing still...
  fir_s16_t fir_filter_;

  bool decimation_filter_{false};
  alignas(16) int16_t fir_filter_coeffecients_[FIR_FILTER_LENGTH];
  alignas(16) int16_t fir_delay_[FIR_FILTER_LENGTH];

  int16_t float_to_q15_(float q, uint32_t shift);
  int8_t generate_q15_fir_coefficients_(int16_t *fir_coeffs, const unsigned int fir_len, const float ft);

};
}  // namespace nabu
}  // namespace esphome

#endif
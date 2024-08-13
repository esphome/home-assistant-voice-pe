////////////////////////////////////////////////////////////////////////////
//                           **** BIQUAD ****                             //
//                     Simple Biquad Filter Library                       //
//                Copyright (c) 2021 - 2022 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// biquad.c

#ifdef USE_ESP_IDF

#include "biquad.h"

// Second-order Lowpass

void biquad_lowpass(BiquadCoefficients *filter, double frequency) {
  double Q = sqrt(0.5), K = tan(M_PI * frequency);
  double norm = 1.0 / (1.0 + K / Q + K * K);

  filter->a0 = K * K * norm;
  filter->a1 = 2 * filter->a0;
  filter->a2 = filter->a0;
  filter->b1 = 2.0 * (K * K - 1.0) * norm;
  filter->b2 = (1.0 - K / Q + K * K) * norm;
}

// Second-order Highpass

void biquad_highpass(BiquadCoefficients *filter, double frequency) {
  double Q = sqrt(0.5), K = tan(M_PI * frequency);
  double norm = 1.0 / (1.0 + K / Q + K * K);

  filter->a0 = norm;
  filter->a1 = -2.0 * norm;
  filter->a2 = filter->a0;
  filter->b1 = 2.0 * (K * K - 1.0) * norm;
  filter->b2 = (1.0 - K / Q + K * K) * norm;
}

// Initialize the specified biquad filter with the given parameters. Note that the "gain" parameter is supplied here
// to save a multiply every time the filter in applied.

void biquad_init(Biquad *f, const BiquadCoefficients *coeffs, float gain) {
  f->coeffs = *coeffs;
  f->coeffs.a0 *= gain;
  f->coeffs.a1 *= gain;
  f->coeffs.a2 *= gain;
  f->in_d1 = f->in_d2 = 0.0F;
  f->out_d1 = f->out_d2 = 0.0F;
  f->first_order = (coeffs->a2 == 0.0F && coeffs->b2 == 0.0F);
}

// Apply the supplied sample to the specified biquad filter, which must have been initialized with biquad_init().

float biquad_apply_sample(Biquad *f, float input) {
  float sum;

  if (f->first_order)
    sum = (input * f->coeffs.a0) + (f->in_d1 * f->coeffs.a1) - (f->coeffs.b1 * f->out_d1);
  else
    sum = (input * f->coeffs.a0) + (f->in_d1 * f->coeffs.a1) + (f->in_d2 * f->coeffs.a2) - (f->coeffs.b1 * f->out_d1) -
          (f->coeffs.b2 * f->out_d2);

  f->out_d2 = f->out_d1;
  f->out_d1 = sum;
  f->in_d2 = f->in_d1;
  f->in_d1 = input;
  return sum;
}

// Apply the supplied buffer to the specified biquad filter, which must have been initialized with biquad_init().

void biquad_apply_buffer(Biquad *f, float *buffer, int num_samples, int stride) {
  if (f->first_order)
    while (num_samples--) {
      float sum = (*buffer * f->coeffs.a0) + (f->in_d1 * f->coeffs.a1) - (f->coeffs.b1 * f->out_d1);
      f->out_d2 = f->out_d1;
      f->in_d2 = f->in_d1;
      f->in_d1 = *buffer;
      *buffer = f->out_d1 = sum;
      buffer += stride;
    }
  else
    while (num_samples--) {
      float sum = (*buffer * f->coeffs.a0) + (f->in_d1 * f->coeffs.a1) + (f->in_d2 * f->coeffs.a2) -
                  (f->coeffs.b1 * f->out_d1) - (f->coeffs.b2 * f->out_d2);
      f->out_d2 = f->out_d1;
      f->in_d2 = f->in_d1;
      f->in_d1 = *buffer;
      *buffer = f->out_d1 = sum;
      buffer += stride;
    }
}
#endif
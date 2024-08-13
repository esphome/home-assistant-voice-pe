#pragma once
////////////////////////////////////////////////////////////////////////////
//                           **** BIQUAD ****                             //
//                     Simple Biquad Filter Library                       //
//                Copyright (c) 2021 - 2022 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// biquad.h

#ifdef USE_ESP_IDF

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

typedef struct {
  float a0, a1, a2, b1, b2;
} BiquadCoefficients;

typedef struct {
  BiquadCoefficients coeffs;  // coefficients
  float in_d1, in_d2;         // delayed input
  float out_d1, out_d2;       // delayed output
  int first_order;            // optimization
} Biquad;

#ifdef __cplusplus
extern "C" {
#endif

void biquad_init(Biquad *f, const BiquadCoefficients *coeffs, float gain);

void biquad_lowpass(BiquadCoefficients *filter, double frequency);
void biquad_highpass(BiquadCoefficients *filter, double frequency);

void biquad_apply_buffer(Biquad *f, float *buffer, int num_samples, int stride);
float biquad_apply_sample(Biquad *f, float input);

#ifdef __cplusplus
}
#endif
#endif
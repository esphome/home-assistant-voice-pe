////////////////////////////////////////////////////////////////////////////
//                         **** RESAMPLER ****                            //
//                     Sinc-based Audio Resampling                        //
//                Copyright (c) 2006 - 2023 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// resampler.h
#pragma once

#ifdef USE_ESP_IDF

#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>

#include "esphome/core/helpers.h"

#define SUBSAMPLE_INTERPOLATE 0x1
#define BLACKMAN_HARRIS 0x2
#define INCLUDE_LOWPASS 0x4

typedef struct {
  int numChannels, numSamples, numFilters, numTaps, inputIndex, flags;
  float *tempFilter, outputOffset;
  float **buffers, **filters;
} Resample;

typedef struct {
  unsigned int input_used, output_generated;
} ResampleResult;

#ifdef __cplusplus
extern "C" {
#endif

Resample *resampleInit(int numChannels, int numTaps, int numFilters, float lowpassRatio, int flags);
ResampleResult resampleProcess(Resample *cxt, const float *const *input, int numInputFrames, float *const *output,
                               int numOutputFrames, float ratio);
ResampleResult resampleProcessInterleaved(Resample *cxt, const float *input, int numInputFrames, float *output,
                                          int numOutputFrames, float ratio);
unsigned int resampleGetRequiredSamples(Resample *cxt, int numOutputFrames, float ratio);
unsigned int resampleGetExpectedOutput(Resample *cxt, int numInputFrames, float ratio);
void resampleAdvancePosition(Resample *cxt, float delta);
float resampleGetPosition(Resample *cxt);
void resampleReset(Resample *cxt);
void resampleFree(Resample *cxt);

#ifdef __cplusplus
}
#endif

#endif

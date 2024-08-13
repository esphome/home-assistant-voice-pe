////////////////////////////////////////////////////////////////////////////
//                         **** RESAMPLER ****                            //
//                     Sinc-based Audio Resampling                        //
//                Copyright (c) 2006 - 2023 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// resampler.c

#ifdef USE_ESP_IDF

#include "resampler.h"

#include "esphome/core/helpers.h"
#include "esp_dsp.h"

static void init_filter(Resample *cxt, float *filter, float fraction, float lowpass_ratio);
static float subsample(Resample *cxt, float *source, float offset);

// Initialize a resampler context with the specified characteristics. The returned context pointer
// is used for all subsequent calls to the resampler (and should not be dereferenced). A NULL
// return indicates an error. For the flags parameter, note that SUBSAMPLE_INTERPOLATE and
// BLACKMAN_HARRIS are recommended for most applications. The parameters are:
//
// numChannels:     the number of audio channels present
//
// numTaps:         the number of taps for each sinc interpolation filter
//                    - must be a multiple of 4, from 4 - 1024 taps
//                    - affects quality by controlling cutoff sharpness of filters
//                    - linearly affects memory usage and CPU load of resampling
//
// numFilters:      the number of sinc filters generated
//                    - must be 2 - 1024
//                    - affects quality of interpolated filtering
//                    - linearly affects memory usage of resampler
//
// lowpassRatio:    enable lowpass by specifying ratio < 1.0 (relative to input samples);
//                   this is required for downsampling, optional otherwise
//
// flags:           mask for optional feature configuration:
//
//   SUBSAMPLE_INTERPOLATE:     interpolate values from adjacent filters
//                                - generally recommended except in special situations
//                                - approximately floats the CPU load
//
//   BLACKMAN_HARRIS:           use 4-term Blackman Harris window function
//                                - generally recommended except in special situations
//                                - if not specified, the default window is Hann (raised cosine)
//                                  which has steeper cutoff but poorer stopband rejection
//
//   INCLUDE_LOWPASS:           include lowpass in sinc interpolation filters
//                                - lowpassRatio specifies frequency as a ratio to input samples
//                                - required for downsampling, optional otherwise
//
// Notes:
//
// 1. The same resampling instance can be used for upsampling, downsampling, or simple (near-unity)
//    resampling (e.g., for asynchronous sample rate conversion, phase shifting, or filtering). The
//    behavior is controlled by the "ratio" parameter during the actual resample processing call.
//    To prevent aliasing, it's important to specify a lowpassRatio if the resampler will be used
//    for any significant degree of downsampling (ratio < 1.0), but the lowpass can also be used
//    independently without any rate conversion (or even upsampling).
//
// 2. When the context is initialized (or reset) the sample histories are filled with silence such
//    that the resampler is ready to generate output immediately. However, this also means that there
//    is an implicit signal delay equal to half the tap length of the sinc filters in samples. If
//    zero delay is desired then that many samples can be ignored, or the resampleAdvancePosition()
//    function can be used to bypass them. Also, at the end of processing an equal length of silence
//    must be appended to the input audio to align the output with the actual input.
//
// 3. Both the number of interpolation filters and the number of taps per filter directly control
//    the fidelity of the resampling. The filter length has an approximately linear affect on the
//    the CPU load consumed by the resampler, and also on the memory requirement (both for the
//    filters themselves and also for the sample history storage). On the other hand, the number
//    of filters allocated primarily affects just the memory footprint (it has little affect on CPU
//    load and so can be large on systems with lots of RAM).

Resample *resampleInit(int numChannels, int numTaps, int numFilters, float lowpassRatio, int flags) {
  Resample *cxt = (Resample *) calloc(1, sizeof(Resample));
  int i;

  if (lowpassRatio > 0.0 && lowpassRatio < 1.0)
    flags |= INCLUDE_LOWPASS;
  else {
    flags &= ~INCLUDE_LOWPASS;
    lowpassRatio = 1.0;
  }

  if ((numTaps & 3) || numTaps <= 0 || numTaps > 1024) {
    fprintf(stderr, "must 4-1024 filter taps, and a multiple of 4!\n");
    return NULL;
  }

  if (numFilters < 2 || numFilters > 1024) {
    fprintf(stderr, "must be 2-1024 filters!\n");
    return NULL;
  }

  cxt->numChannels = numChannels;
  cxt->numSamples = numTaps * 16;
  cxt->numFilters = numFilters;
  cxt->numTaps = numTaps;
  cxt->flags = flags;

  // note that we actually have one more than the specified number of filters
  esphome::ExternalRAMAllocator<float> float_allocator(esphome::ExternalRAMAllocator<float>::ALLOW_FAILURE);
  cxt->filters = (float **) calloc(cxt->numFilters + 1, sizeof(float *));
  cxt->tempFilter = float_allocator.allocate(numTaps);
  // cxt->tempFilter = malloc (numTaps * sizeof (float));

  for (i = 0; i <= cxt->numFilters; ++i) {
    cxt->filters[i] = float_allocator.allocate(cxt->numTaps);
    memset(cxt->filters[i], 0, cxt->numTaps * sizeof(float));
    // cxt->filters [i] = calloc (cxt->numTaps, sizeof (float));
    init_filter(cxt, cxt->filters[i], (float) i / cxt->numFilters, lowpassRatio);
  }

  free(cxt->tempFilter);
  cxt->tempFilter = NULL;
  cxt->buffers = (float **) calloc(numChannels, sizeof(float *));

  for (i = 0; i < numChannels; ++i) {
    cxt->buffers[i] = float_allocator.allocate(cxt->numSamples);
    memset(cxt->buffers[i], 0, cxt->numSamples * sizeof(float));
    // cxt->buffers [i] = calloc (cxt->numSamples, sizeof (float));
  }

  cxt->outputOffset = numTaps / 2;
  cxt->inputIndex = numTaps;

  return cxt;
}

// Reset a resampler context to its initialized state. Specifically, any history is discarded
// and this should be used when an audio "flush" or other discontinuity occurs.

void resampleReset(Resample *cxt) {
  int i;

  for (i = 0; i < cxt->numChannels; ++i)
    memset(cxt->buffers[i], 0, cxt->numSamples * sizeof(float));

  cxt->outputOffset = cxt->numTaps / 2;
  cxt->inputIndex = cxt->numTaps;
}

// Run the resampler context at the specified output ratio and return both the number of input
// samples consumed and output samples generated (in the ResampleResult structure). Over time
// the average number of output samples will be equal to the number of input samples multiplied
// by the given ratio, but of course in a single call only an integer number of samples can be
// generated. The numInputFrames parameter indicates the number of samples available at "input"
// and the numOutputFrames indicates the number of samples at "output" that can be written. The
// resampling proceeds until EITHER the input is exhausted or space at the output is exhausted
// (there is no other limit).
//
// This is the "non-interleaved" version of the resampler where the audio sample buffers for
// different channels are passed in as an array of float pointers. There is also an
// "interleaved" version (see below).

ResampleResult resampleProcess(Resample *cxt, const float *const *input, int numInputFrames, float *const *output,
                               int numOutputFrames, float ratio) {
  int half_taps = cxt->numTaps / 2, i;
  ResampleResult res = {0, 0};

  while (numOutputFrames > 0) {
    if (cxt->outputOffset >= cxt->inputIndex - half_taps) {
      if (numInputFrames > 0) {
        if (cxt->inputIndex == cxt->numSamples) {
          for (i = 0; i < cxt->numChannels; ++i)
            memmove(cxt->buffers[i], cxt->buffers[i] + cxt->numSamples - cxt->numTaps, cxt->numTaps * sizeof(float));

          cxt->outputOffset -= cxt->numSamples - cxt->numTaps;
          cxt->inputIndex -= cxt->numSamples - cxt->numTaps;
        }

        for (i = 0; i < cxt->numChannels; ++i)
          cxt->buffers[i][cxt->inputIndex] = input[i][res.input_used];

        cxt->inputIndex++;
        res.input_used++;
        numInputFrames--;
      } else
        break;
    } else {
      for (i = 0; i < cxt->numChannels; ++i)
        output[i][res.output_generated] = subsample(cxt, cxt->buffers[i], cxt->outputOffset);

      cxt->outputOffset += (1.0 / ratio);
      res.output_generated++;
      numOutputFrames--;
    }
  }

  return res;
}

// This is the "interleaved" version of the resampler where the audio samples for different
// channels are passed in sequence in a single buffer. There is also a "non-interleaved"
// version for independent buffers, which is otherwise identical (see above).

ResampleResult resampleProcessInterleaved(Resample *cxt, const float *input, int numInputFrames, float *output,
                                          int numOutputFrames, float ratio) {
  int half_taps = cxt->numTaps / 2, i;
  ResampleResult res = {0, 0};

  while (numOutputFrames > 0) {
    if (cxt->outputOffset >= cxt->inputIndex - half_taps) {
      if (numInputFrames > 0) {
        if (cxt->inputIndex == cxt->numSamples) {
          for (i = 0; i < cxt->numChannels; ++i)
            memmove(cxt->buffers[i], cxt->buffers[i] + cxt->numSamples - cxt->numTaps, cxt->numTaps * sizeof(float));

          cxt->outputOffset -= cxt->numSamples - cxt->numTaps;
          cxt->inputIndex -= cxt->numSamples - cxt->numTaps;
        }

        for (i = 0; i < cxt->numChannels; ++i)
          cxt->buffers[i][cxt->inputIndex] = *input++;

        cxt->inputIndex++;
        res.input_used++;
        numInputFrames--;
      } else
        break;
    } else {
      for (i = 0; i < cxt->numChannels; ++i)
        *output++ = subsample(cxt, cxt->buffers[i], cxt->outputOffset);

      cxt->outputOffset += (1.0 / ratio);
      res.output_generated++;
      numOutputFrames--;
    }
  }

  return res;
}

// These two functions are not required for any application, but might be useful. Essentially
// they allow a "dry run" of the resampler to determine beforehand how many input samples
// would be consumed to generate a given output, or how many samples would be generated with
// a given input.
//
// Note that there is a tricky edge-case here for ratios just over 1.0. If a query is made as
// to how many input samples are required to generate a given output, that does NOT necessarily
// mean that exactly that many samples will be generated with the indicated input (specifically
// an extra sample might be generated). Therefore it is important to restrict the output with
// numOutputFrames if an exact output count is desired (don't just assume the input count can
// exactly determine the output count).

unsigned int resampleGetRequiredSamples(Resample *cxt, int numOutputFrames, float ratio) {
  int half_taps = cxt->numTaps / 2;
  int input_index = cxt->inputIndex;
  float offset = cxt->outputOffset;
  ResampleResult res = {0, 0};

  while (numOutputFrames > 0) {
    if (offset >= input_index - half_taps) {
      if (input_index == cxt->numSamples) {
        offset -= cxt->numSamples - cxt->numTaps;
        input_index -= cxt->numSamples - cxt->numTaps;
      }

      input_index++;
      res.input_used++;
    } else {
      offset += (1.0 / ratio);
      numOutputFrames--;
    }
  }

  return res.input_used;
}

unsigned int resampleGetExpectedOutput(Resample *cxt, int numInputFrames, float ratio) {
  int half_taps = cxt->numTaps / 2;
  int input_index = cxt->inputIndex;
  float offset = cxt->outputOffset;
  ResampleResult res = {0, 0};

  while (1) {
    if (offset >= input_index - half_taps) {
      if (numInputFrames > 0) {
        if (input_index == cxt->numSamples) {
          offset -= cxt->numSamples - cxt->numTaps;
          input_index -= cxt->numSamples - cxt->numTaps;
        }

        input_index++;
        numInputFrames--;
      } else
        break;
    } else {
      offset += (1.0 / ratio);
      res.output_generated++;
    }
  }

  return res.output_generated;
}

// Advance the resampler output without generating any output, with the units referenced
// to the input sample array. This can be used to temporally align the output to the input
// (by specifying half the sinc filter tap width), and it can also be used to introduce a
// phase shift. The resampler cannot be reversed.

void resampleAdvancePosition(Resample *cxt, float delta) {
  if (delta < 0.0)
    fprintf(stderr, "resampleAdvancePosition() can only advance forward!\n");
  else
    cxt->outputOffset += delta;
}

// Get the subsample position of the resampler. This is initialized to zero when the
// resampler is started (or reset) and moves around zero as the resampler processes
// audio. Obtaining this value is generally not required, but can be useful for
// applications that need accurate phase information from the resampler such as
// asynchronous sample rate converter (ASRC) implementations. The units are relative
// to the input samples, and a negative value indicates that an output sample is
// ready (i.e., can be generated with no further input read).
//
// To fully understand the meaning of the position value the following C-like
// pseudo-code for the resampler is presented. Note that this code has the length
// of the sinc filters and the actual interpolation abstracted away (like they
// abstracted away from the user of the library):
//
// while (numOutputFrames > 0) {
//     if (position < 0.0) {
//         write (output);
//         numOutputFrames--;
//         position += (1.0 / ratio);
//     }
//     else if (numInputFrames > 0) {
//         read (input);
//         numInputFrames--;
//         position -= 1.0;
//     }
//     else
//         break;
// }

float resampleGetPosition(Resample *cxt) { return cxt->outputOffset + (cxt->numTaps / 2.0) - cxt->inputIndex; }

// Free all resources associated with the resampler context, including the context pointer
// itself. Do not use the context after this call.

void resampleFree(Resample *cxt) {
  int i;

  for (i = 0; i <= cxt->numFilters; ++i)
    free(cxt->filters[i]);

  free(cxt->filters);

  for (i = 0; i < cxt->numChannels; ++i)
    free(cxt->buffers[i]);

  free(cxt->buffers);
  free(cxt);
}

// This is the basic convolution operation that is the core of the resampler and utilizes the
// bulk of the CPU load (assuming reasonably long filters). The first version is the canonical
// form, followed by three variations that may or may not be faster depending on your compiler,
// options, and system. Try 'em and use the fastest, or rewrite them using SIMD. Note that on
// gcc and clang, -Ofast can make a huge difference.

#if 1  // Version 1 (canonical)
// static float apply_filter (float *A, float *B, int num_taps)
// {
//     float sum = 0.0;

//     do sum += *A++ * *B++;
//     while (--num_taps);

//     return sum;
// }
static float apply_filter(float *A, float *B, int num_taps) {
  float sum;
  dsps_dotprod_f32_aes3(A, B, &sum, num_taps);
  //   dsps_dotprod_f32_ansi(A, B, &sum, num_taps);
  return sum;
}
#endif

#if 0  // Version 2 (2x unrolled loop)
static float apply_filter (float *A, float *B, int num_taps)
{
    int num_loops = num_taps >> 1;
    float sum = 0.0;

    do {
        sum += (A[0] * B[0]) + (A[1] * B[1]);
        A += 2; B += 2;
    } while (--num_loops);

    return sum;
}
#endif

#if 0  // Version 3 (4x unrolled loop)
static float apply_filter (float *A, float *B, int num_taps)
{
    int num_loops = num_taps >> 2;
    float sum = 0.0;

    do {
        sum += (A[0] * B[0]) + (A[1] * B[1]) + (A[2] * B[2]) + (A[3] * B[3]);
        A += 4; B += 4;
    } while (--num_loops);

    return sum;
}
#endif

#if 0  // Version 4 (outside-in order, may be more accurate)
static float apply_filter (float *A, float *B, int num_taps)
{
    int i = num_taps - 1;
    float sum = 0.0;

    do {
        sum += (A[0] * B[0]) + (A[i] * B[i]);
        A++; B++;
    } while ((i -= 2) > 0);

    return sum;
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979324
#endif

static void init_filter(Resample *cxt, float *filter, float fraction, float lowpass_ratio) {
  const float a0 = 0.35875;
  const float a1 = 0.48829;
  const float a2 = 0.14128;
  const float a3 = 0.01168;
  float filter_sum = 0.0;
  int i;

  // "dist" is the absolute distance from the sinc maximum to the filter tap to be calculated, in radians
  // "ratio" is that distance divided by half the tap count such that it reaches π at the window extremes

  // Note that with this scaling, the odd terms of the Blackman-Harris calculation appear to be negated
  // with respect to the reference formula version.

  for (i = 0; i < cxt->numTaps; ++i) {
    float dist = fabs((cxt->numTaps / 2 - 1) + fraction - i) * M_PI;
    float ratio = dist / (cxt->numTaps / 2);
    float value;

    if (dist != 0.0) {
      value = sin(dist * lowpass_ratio) / (dist * lowpass_ratio);

      if (cxt->flags & BLACKMAN_HARRIS)
        value *= a0 + a1 * cos(ratio) + a2 * cos(2 * ratio) + a3 * cos(3 * ratio);
      else
        value *= 0.5 * (1.0 + cos(ratio));  // Hann window
    } else
      value = 1.0;

    filter_sum += cxt->tempFilter[i] = value;
  }

  // filter should have unity DC gain

  float scaler = 1.0 / filter_sum, error = 0.0;

  for (i = cxt->numTaps / 2; i < cxt->numTaps; i = cxt->numTaps - i - (i >= cxt->numTaps / 2)) {
    filter[i] = (cxt->tempFilter[i] *= scaler) - error;
    error += filter[i] - cxt->tempFilter[i];
  }
}

static float subsample_no_interpolate(Resample *cxt, float *source, float offset) {
  source += (int) floor(offset);
  offset -= floor(offset);

  if (offset == 0.0 && !(cxt->flags & INCLUDE_LOWPASS))
    return *source;

  return apply_filter(cxt->filters[(int) floor(offset * cxt->numFilters + 0.5)], source - cxt->numTaps / 2 + 1,
                      cxt->numTaps);
}

static float subsample_interpolate(Resample *cxt, float *source, float offset) {
  float sum1, sum2;
  int i;

  source += (int) floor(offset);
  offset -= floor(offset);

  if (offset == 0.0 && !(cxt->flags & INCLUDE_LOWPASS))
    return *source;

  i = (int) floor(offset *= cxt->numFilters);
  sum1 = apply_filter(cxt->filters[i], source - cxt->numTaps / 2 + 1, cxt->numTaps);

  if ((offset -= i) == 0.0 && !(cxt->flags & INCLUDE_LOWPASS))
    return sum1;

  sum2 = apply_filter(cxt->filters[i + 1], source - cxt->numTaps / 2 + 1, cxt->numTaps);

  return sum2 * offset + sum1 * (1.0 - offset);
}

static float subsample(Resample *cxt, float *source, float offset) {
  if (cxt->flags & SUBSAMPLE_INTERPOLATE)
    return subsample_interpolate(cxt, source, offset);
  else
    return subsample_no_interpolate(cxt, source, offset);
}

#endif
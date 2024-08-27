/* ***** BEGIN LICENSE BLOCK *****
 * Version: RCSL 1.0/RPSL 1.0
 *
 * Portions Copyright (c) 1995-2002 RealNetworks, Inc. All Rights Reserved.
 *
 * The contents of this file, and the files included with this file, are
 * subject to the current version of the RealNetworks Public Source License
 * Version 1.0 (the "RPSL") available at
 * http://www.helixcommunity.org/content/rpsl unless you have licensed
 * the file under the RealNetworks Community Source License Version 1.0
 * (the "RCSL") available at http://www.helixcommunity.org/content/rcsl,
 * in which case the RCSL will apply. You may also obtain the license terms
 * directly from RealNetworks.  You may not use this file except in
 * compliance with the RPSL or, if you have a valid RCSL with RealNetworks
 * applicable to this file, the RCSL.  Please see the applicable RPSL or
 * RCSL for the rights, obligations and limitations governing use of the
 * contents of the file.
 *
 * This file is part of the Helix DNA Technology. RealNetworks is the
 * developer of the Original Code and owns the copyrights in the portions
 * it created.
 *
 * This file, and the files included with this file, is distributed and made
 * available on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND REALNETWORKS HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 *
 * Technology Compatibility Kit Test Suite(s) Location:
 *    http://www.helixcommunity.org/content/tck
 *
 * Contributor(s):
 *
 * ***** END LICENSE BLOCK ***** */

/**************************************************************************************
 * Fixed-point MP3 decoder
 * Jon Recker (jrecker@real.com), Ken Cooke (kenc@real.com)
 * June 2003
 *
 **************************************************************************************/

#ifdef USE_ESP_IDF

#include "mp3_decoder.h"
#include "esphome/core/helpers.h"

/* indexing = [version][samplerate index]
 * sample rate of frame (Hz)
 */
const int samplerateTab[3][3] = {
    {44100, 48000, 32000}, /* MPEG-1 */
    {22050, 24000, 16000}, /* MPEG-2 */
    {11025, 12000, 8000},  /* MPEG-2.5 */
};

/* indexing = [version][layer][bitrate index]
 * bitrate (kbps) of frame
 *   - bitrate index == 0 is "free" mode (bitrate determined on the fly by
 *       counting bits between successive sync words)
 */
const short bitrateTab[3][3][15] = {
    {
        /* MPEG-1 */
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448}, /* Layer 1 */
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},    /* Layer 2 */
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},     /* Layer 3 */
    },
    {
        /* MPEG-2 */
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256}, /* Layer 1 */
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},      /* Layer 2 */
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},      /* Layer 3 */
    },
    {
        /* MPEG-2.5 */
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256}, /* Layer 1 */
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},      /* Layer 2 */
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},      /* Layer 3 */
    },
};

/* indexing = [version][layer]
 * number of samples in one frame (per channel)
 */
const short samplesPerFrameTab[3][3] = {
    {384, 1152, 1152}, /* MPEG1 */
    {384, 1152, 576},  /* MPEG2 */
    {384, 1152, 576},  /* MPEG2.5 */
};

/* layers 1, 2, 3 */
const short bitsPerSlotTab[3] = {32, 8, 8};

/* indexing = [version][mono/stereo]
 * number of bytes in side info section of bitstream
 */
const short sideBytesTab[3][2] = {
    {17, 32}, /* MPEG-1:   mono, stereo */
    {9, 17},  /* MPEG-2:   mono, stereo */
    {9, 17},  /* MPEG-2.5: mono, stereo */
};

/* indexing = [version][sampleRate][bitRate]
 * for layer3, nSlots = floor(samps/frame * bitRate / sampleRate / 8)
 *   - add one pad slot if necessary
 */
const short slotTab[3][3][15] = {
    {
        /* MPEG-1 */
        {0, 104, 130, 156, 182, 208, 261, 313, 365, 417, 522, 626, 731, 835, 1044},   /* 44 kHz */
        {0, 96, 120, 144, 168, 192, 240, 288, 336, 384, 480, 576, 672, 768, 960},     /* 48 kHz */
        {0, 144, 180, 216, 252, 288, 360, 432, 504, 576, 720, 864, 1008, 1152, 1440}, /* 32 kHz */
    },
    {
        /* MPEG-2 */
        {0, 26, 52, 78, 104, 130, 156, 182, 208, 261, 313, 365, 417, 470, 522},  /* 22 kHz */
        {0, 24, 48, 72, 96, 120, 144, 168, 192, 240, 288, 336, 384, 432, 480},   /* 24 kHz */
        {0, 36, 72, 108, 144, 180, 216, 252, 288, 360, 432, 504, 576, 648, 720}, /* 16 kHz */
    },
    {
        /* MPEG-2.5 */
        {0, 52, 104, 156, 208, 261, 313, 365, 417, 522, 626, 731, 835, 940, 1044},    /* 11 kHz */
        {0, 48, 96, 144, 192, 240, 288, 336, 384, 480, 576, 672, 768, 864, 960},      /* 12 kHz */
        {0, 72, 144, 216, 288, 360, 432, 504, 576, 720, 864, 1008, 1152, 1296, 1440}, /*  8 kHz */
    },
};

/* indexing = [version][sampleRate][long (.l) or short (.s) block]
 *   sfBandTable[v][s].l[cb] = index of first bin in critical band cb (long
 * blocks) sfBandTable[v][s].s[cb] = index of first bin in critical band cb
 * (short blocks)
 */
const SFBandTable sfBandTable[3][3] = {
    {/* MPEG-1 (44, 48, 32 kHz) */
     {{0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134, 162, 196, 238, 288, 342, 418, 576},
      {0, 4, 8, 12, 16, 22, 30, 40, 52, 66, 84, 106, 136, 192}},
     {{0, 4, 8, 12, 16, 20, 24, 30, 36, 42, 50, 60, 72, 88, 106, 128, 156, 190, 230, 276, 330, 384, 576},
      {0, 4, 8, 12, 16, 22, 28, 38, 50, 64, 80, 100, 126, 192}},
     {{0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 54, 66, 82, 102, 126, 156, 194, 240, 296, 364, 448, 550, 576},
      {0, 4, 8, 12, 16, 22, 30, 42, 58, 78, 104, 138, 180, 192}}},

    {
        /* MPEG-2 (22, 24, 16 kHz) */
        {{0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
         {0, 4, 8, 12, 18, 24, 32, 42, 56, 74, 100, 132, 174, 192}},
        {{0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 114, 136, 162, 194, 232, 278, 332, 394, 464, 540, 576},
         {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 136, 180, 192}},
        {{0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
         {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192}},
    },

    {
        /* MPEG-2.5 (11, 12, 8 kHz) */
        {{0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
         {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192}},
        {{0, 6, 12, 18, 24, 30, 36, 44, 54, 66, 80, 96, 116, 140, 168, 200, 238, 284, 336, 396, 464, 522, 576},
         {0, 4, 8, 12, 18, 26, 36, 48, 62, 80, 104, 134, 174, 192}},
        {{0, 12, 24, 36, 48, 60, 72, 88, 108, 132, 160, 192, 232, 280, 336, 400, 476, 566, 568, 570, 572, 574, 576},
         {0, 8, 16, 24, 36, 52, 72, 96, 124, 160, 162, 164, 166, 192}},
    },
};

const uint32_t imdctWin[4][36] = {
    {
        0x02aace8b, 0x07311c28, 0x0a868fec, 0x0c913b52, 0x0d413ccd, 0x0c913b52, 0x0a868fec, 0x07311c28, 0x02aace8b,
        0xfd16d8dd, 0xf6a09e66, 0xef7a6275, 0xe7dbc161, 0xe0000000, 0xd8243e9f, 0xd0859d8b, 0xc95f619a, 0xc2e92723,
        0xbd553175, 0xb8cee3d8, 0xb5797014, 0xb36ec4ae, 0xb2bec333, 0xb36ec4ae, 0xb5797014, 0xb8cee3d8, 0xbd553175,
        0xc2e92723, 0xc95f619a, 0xd0859d8b, 0xd8243e9f, 0xe0000000, 0xe7dbc161, 0xef7a6275, 0xf6a09e66, 0xfd16d8dd,
    },
    {
        0x02aace8b, 0x07311c28, 0x0a868fec, 0x0c913b52, 0x0d413ccd, 0x0c913b52, 0x0a868fec, 0x07311c28, 0x02aace8b,
        0xfd16d8dd, 0xf6a09e66, 0xef7a6275, 0xe7dbc161, 0xe0000000, 0xd8243e9f, 0xd0859d8b, 0xc95f619a, 0xc2e92723,
        0xbd44ef14, 0xb831a052, 0xb3aa3837, 0xafb789a4, 0xac6145bb, 0xa9adecdc, 0xa864491f, 0xad1868f0, 0xb8431f49,
        0xc8f42236, 0xdda8e6b1, 0xf47755dc, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    },
    {
        0x07311c28, 0x0d413ccd, 0x07311c28, 0xf6a09e66, 0xe0000000, 0xc95f619a, 0xb8cee3d8, 0xb2bec333, 0xb8cee3d8,
        0xc95f619a, 0xe0000000, 0xf6a09e66, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
    },
    {
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x028e9709, 0x04855ec0, 0x026743a1,
        0xfcde2c10, 0xf515dc82, 0xec93e53b, 0xe4c880f8, 0xdd5d0b08, 0xd63510b7, 0xcf5e834a, 0xc8e6b562, 0xc2da4105,
        0xbd553175, 0xb8cee3d8, 0xb5797014, 0xb36ec4ae, 0xb2bec333, 0xb36ec4ae, 0xb5797014, 0xb8cee3d8, 0xbd553175,
        0xc2e92723, 0xc95f619a, 0xd0859d8b, 0xd8243e9f, 0xe0000000, 0xe7dbc161, 0xef7a6275, 0xf6a09e66, 0xfd16d8dd,
    },
};

/* indexing = [mid-side off/on][intensity scale factor]
 * format = Q30, range = [0.0, 1.414]
 *
 * mid-side off:
 *   ISFMpeg1[0][i] = tan(i*pi/12) / [1 + tan(i*pi/12)]  (left scalefactor)
 *                  =      1       / [1 + tan(i*pi/12)]  (right scalefactor)
 *
 * mid-side on:
 *   ISFMpeg1[1][i] = sqrt(2) * ISFMpeg1[0][i]
 *
 * output L = ISFMpeg1[midSide][isf][0] * input L
 * output R = ISFMpeg1[midSide][isf][1] * input L
 *
 * obviously left scalefactor + right scalefactor = 1 (m-s off) or sqrt(2) (m-s
 * on) so just store left and calculate right as 1 - left (can derive as right =
 * ISFMpeg1[x][6] - left)
 *
 * if mid-side enabled, multiply joint stereo scale factors by sqrt(2)
 *   - we scaled whole spectrum by 1/sqrt(2) in Dequant for the M+S/sqrt(2) in
 * MidSideProc
 *   - but the joint stereo part of the spectrum doesn't need this, so we have
 * to undo it
 *
 * if scale factor is and illegal intensity position, this becomes a passthrough
 *   - gain = [1, 0] if mid-side off, since L is coded directly and R = 0 in
 * this region
 *   - gain = [1, 1] if mid-side on, since L = (M+S)/sqrt(2), R = (M-S)/sqrt(2)
 *     - and since S = 0 in the joint stereo region (above NZB right) then L = R
 * = M * 1.0
 */
const int ISFMpeg1[2][7] = {{0x00000000, 0x0d8658ba, 0x176cf5d0, 0x20000000, 0x28930a2f, 0x3279a745, 0x40000000},
                            {0x00000000, 0x13207f5c, 0x2120fb83, 0x2d413ccc, 0x39617e16, 0x4761fa3d, 0x5a827999}};

/* indexing = [intensity scale on/off][mid-side off/on][intensity scale factor]
 * format = Q30, range = [0.0, 1.414]
 *
 * if (isf == 0)                 kl = 1.0             kr = 1.0
 * else if (isf & 0x01 == 0x01)  kl = i0^((isf+1)/2), kr = 1.0
 * else if (isf & 0x01 == 0x00)  kl = 1.0,            kr = i0^(isf/2)
 *
 * if (intensityScale == 1)      i0 = 1/sqrt(2)       = 0x2d413ccc (Q30)
 * else                          i0 = 1/sqrt(sqrt(2)) = 0x35d13f32 (Q30)
 *
 * see comments for ISFMpeg1 (just above) regarding scaling, sqrt(2), etc.
 *
 * compress the MPEG2 table using the obvious identities above...
 * for isf = [0, 1, 2, ... 30], let sf = table[(isf+1) >> 1]
 *   - if isf odd,  L = sf*L,     R = tab[0]*R
 *   - if isf even, L = tab[0]*L, R = sf*R
 */
const int ISFMpeg2[2][2][16] = {{
                                    {
                                        /* intensityScale off, mid-side off */
                                        0x40000000,
                                        0x35d13f32,
                                        0x2d413ccc,
                                        0x260dfc14,
                                        0x1fffffff,
                                        0x1ae89f99,
                                        0x16a09e66,
                                        0x1306fe0a,
                                        0x0fffffff,
                                        0x0d744fcc,
                                        0x0b504f33,
                                        0x09837f05,
                                        0x07ffffff,
                                        0x06ba27e6,
                                        0x05a82799,
                                        0x04c1bf82,
                                    },
                                    {
                                        /* intensityScale off, mid-side on */
                                        0x5a827999,
                                        0x4c1bf827,
                                        0x3fffffff,
                                        0x35d13f32,
                                        0x2d413ccc,
                                        0x260dfc13,
                                        0x1fffffff,
                                        0x1ae89f99,
                                        0x16a09e66,
                                        0x1306fe09,
                                        0x0fffffff,
                                        0x0d744fcc,
                                        0x0b504f33,
                                        0x09837f04,
                                        0x07ffffff,
                                        0x06ba27e6,
                                    },
                                },
                                {{
                                     /* intensityScale on, mid-side off */
                                     0x40000000,
                                     0x2d413ccc,
                                     0x20000000,
                                     0x16a09e66,
                                     0x10000000,
                                     0x0b504f33,
                                     0x08000000,
                                     0x05a82799,
                                     0x04000000,
                                     0x02d413cc,
                                     0x02000000,
                                     0x016a09e6,
                                     0x01000000,
                                     0x00b504f3,
                                     0x00800000,
                                     0x005a8279,
                                 },
                                 /* intensityScale on, mid-side on */
                                 {
                                     0x5a827999,
                                     0x3fffffff,
                                     0x2d413ccc,
                                     0x1fffffff,
                                     0x16a09e66,
                                     0x0fffffff,
                                     0x0b504f33,
                                     0x07ffffff,
                                     0x05a82799,
                                     0x03ffffff,
                                     0x02d413cc,
                                     0x01ffffff,
                                     0x016a09e6,
                                     0x00ffffff,
                                     0x00b504f3,
                                     0x007fffff,
                                 }}};

/* indexing = [intensity scale on/off][left/right]
 * format = Q30, range = [0.0, 1.414]
 *
 * illegal intensity position scalefactors (see comments on ISFMpeg1)
 */
const int ISFIIP[2][2] = {
    {0x40000000, 0x00000000}, /* mid-side off */
    {0x40000000, 0x40000000}, /* mid-side on */
};

const unsigned char uniqueIDTab[8] = {0x5f, 0x4b, 0x43, 0x5f, 0x5f, 0x4a, 0x52, 0x5f};

/* anti-alias coefficients - see spec Annex B, table 3-B.9
 *   csa[0][i] = CSi, csa[1][i] = CAi
 * format = Q31
 */
const uint32_t csa[8][2] = {
    {0x6dc253f0, 0xbe2500aa}, {0x70dcebe4, 0xc39e4949}, {0x798d6e73, 0xd7e33f4a}, {0x7ddd40a7, 0xe8b71176},
    {0x7f6d20b7, 0xf3e4fe2f}, {0x7fe47e40, 0xfac1a3c7}, {0x7ffcb263, 0xfe2ebdc6}, {0x7fffc694, 0xff86c25d},
};

/* format = Q30, range = [0.0981, 1.9976]
 *
 * n = 16;
 * k = 0;
 * for(i=0; i<5; i++, n=n/2) {
 *   for(p=0; p<n; p++, k++) {
 *     t = (PI / (4*n)) * (2*p + 1);
 *     coef32[k] = 2.0 * cos(t);
 *   }
 * }
 * coef32[30] *= 0.5;	/ *** for initial back butterfly (i.e. two-point DCT)
 * *** /
 */
const int coef32[31] = {
    0x7fd8878d, 0x7e9d55fc, 0x7c29fbee, 0x78848413, 0x73b5ebd0, 0x6dca0d14, 0x66cf811f, 0x5ed77c89,
    0x55f5a4d2, 0x4c3fdff3, 0x41ce1e64, 0x36ba2013, 0x2b1f34eb, 0x1f19f97b, 0x12c8106e, 0x0647d97c,
    0x7f62368f, 0x7a7d055b, 0x70e2cbc6, 0x62f201ac, 0x5133cc94, 0x3c56ba70, 0x25280c5d, 0x0c8bd35e,
    0x7d8a5f3f, 0x6a6d98a4, 0x471cece6, 0x18f8b83c, 0x7641af3c, 0x30fbc54d, 0x2d413ccc,
};

/* format = Q30, right shifted by 12 (sign bits only in top 12 - undo this when
 * rounding to short) this is to enable early-terminating multiplies on ARM
 * range = [-1.144287109, 1.144989014]
 * max gain of filter (per output sample) ~= 2.731
 *
 * new (properly sign-flipped) values
 *  - these actually are correct to 32 bits, (floating-pt coefficients in spec
 *      chosen such that only ~20 bits are required)
 *
 * Reordering - see table 3-B.3 in spec (appendix B)
 *
 * polyCoef[i] =
 *   D[ 0, 32, 64, ... 480],   i = [  0, 15]
 *   D[ 1, 33, 65, ... 481],   i = [ 16, 31]
 *   D[ 2, 34, 66, ... 482],   i = [ 32, 47]
 *     ...
 *   D[15, 47, 79, ... 495],   i = [240,255]
 *
 * also exploits symmetry: D[i] = -D[512 - i], for i = [1, 255]
 *
 * polyCoef[256, 257, ... 263] are for special case of sample 16 (out of 0)
 *   see PolyphaseStereo() and PolyphaseMono()
 */
const uint32_t polyCoef[264] = {
    /* shuffled vs. original from 0, 1, ... 15 to 0, 15, 2, 13, ... 14, 1 */
    0x00000000, 0x00000074, 0x00000354, 0x0000072c, 0x00001fd4, 0x00005084, 0x000066b8, 0x000249c4, 0x00049478,
    0xfffdb63c, 0x000066b8, 0xffffaf7c, 0x00001fd4, 0xfffff8d4, 0x00000354, 0xffffff8c, 0xfffffffc, 0x00000068,
    0x00000368, 0x00000644, 0x00001f40, 0x00004ad0, 0x00005d1c, 0x00022ce0, 0x000493c0, 0xfffd9960, 0x00006f78,
    0xffffa9cc, 0x0000203c, 0xfffff7e4, 0x00000340, 0xffffff84, 0xfffffffc, 0x00000060, 0x00000378, 0x0000056c,
    0x00001e80, 0x00004524, 0x000052a0, 0x00020ffc, 0x000491a0, 0xfffd7ca0, 0x00007760, 0xffffa424, 0x00002080,
    0xfffff6ec, 0x00000328, 0xffffff74, 0xfffffffc, 0x00000054, 0x00000384, 0x00000498, 0x00001d94, 0x00003f7c,
    0x00004744, 0x0001f32c, 0x00048e18, 0xfffd6008, 0x00007e70, 0xffff9e8c, 0x0000209c, 0xfffff5ec, 0x00000310,
    0xffffff68, 0xfffffffc, 0x0000004c, 0x0000038c, 0x000003d0, 0x00001c78, 0x000039e4, 0x00003b00, 0x0001d680,
    0x00048924, 0xfffd43ac, 0x000084b0, 0xffff990c, 0x00002094, 0xfffff4e4, 0x000002f8, 0xffffff5c, 0xfffffffc,
    0x00000044, 0x00000390, 0x00000314, 0x00001b2c, 0x0000345c, 0x00002ddc, 0x0001ba04, 0x000482d0, 0xfffd279c,
    0x00008a20, 0xffff93a4, 0x0000206c, 0xfffff3d4, 0x000002dc, 0xffffff4c, 0xfffffffc, 0x00000040, 0x00000390,
    0x00000264, 0x000019b0, 0x00002ef0, 0x00001fd4, 0x00019dc8, 0x00047b1c, 0xfffd0be8, 0x00008ecc, 0xffff8e64,
    0x00002024, 0xfffff2c0, 0x000002c0, 0xffffff3c, 0xfffffff8, 0x00000038, 0x0000038c, 0x000001bc, 0x000017fc,
    0x0000299c, 0x000010e8, 0x000181d8, 0x0004720c, 0xfffcf09c, 0x000092b4, 0xffff894c, 0x00001fc0, 0xfffff1a4,
    0x000002a4, 0xffffff2c, 0xfffffff8, 0x00000034, 0x00000380, 0x00000120, 0x00001618, 0x00002468, 0x00000118,
    0x00016644, 0x000467a4, 0xfffcd5cc, 0x000095e0, 0xffff8468, 0x00001f44, 0xfffff084, 0x00000284, 0xffffff18,
    0xfffffff8, 0x0000002c, 0x00000374, 0x00000090, 0x00001400, 0x00001f58, 0xfffff068, 0x00014b14, 0x00045bf0,
    0xfffcbb88, 0x00009858, 0xffff7fbc, 0x00001ea8, 0xffffef60, 0x00000268, 0xffffff04, 0xfffffff8, 0x00000028,
    0x0000035c, 0x00000008, 0x000011ac, 0x00001a70, 0xffffded8, 0x00013058, 0x00044ef8, 0xfffca1d8, 0x00009a1c,
    0xffff7b54, 0x00001dfc, 0xffffee3c, 0x0000024c, 0xfffffef0, 0xfffffff4, 0x00000024, 0x00000340, 0xffffff8c,
    0x00000f28, 0x000015b0, 0xffffcc70, 0x0001161c, 0x000440bc, 0xfffc88d8, 0x00009b3c, 0xffff7734, 0x00001d38,
    0xffffed18, 0x0000022c, 0xfffffedc, 0xfffffff4, 0x00000020, 0x00000320, 0xffffff1c, 0x00000c68, 0x0000111c,
    0xffffb92c, 0x0000fc6c, 0x00043150, 0xfffc708c, 0x00009bb8, 0xffff7368, 0x00001c64, 0xffffebf4, 0x00000210,
    0xfffffec4, 0xfffffff0, 0x0000001c, 0x000002f4, 0xfffffeb4, 0x00000974, 0x00000cb8, 0xffffa518, 0x0000e350,
    0x000420b4, 0xfffc5908, 0x00009b9c, 0xffff6ff4, 0x00001b7c, 0xffffead0, 0x000001f4, 0xfffffeac, 0xfffffff0,
    0x0000001c, 0x000002c4, 0xfffffe58, 0x00000648, 0x00000884, 0xffff9038, 0x0000cad0, 0x00040ef8, 0xfffc425c,
    0x00009af0, 0xffff6ce0, 0x00001a88, 0xffffe9b0, 0x000001d4, 0xfffffe94, 0xffffffec, 0x00000018, 0x0000028c,
    0xfffffe04, 0x000002e4, 0x00000480, 0xffff7a90, 0x0000b2fc, 0x0003fc28, 0xfffc2c90, 0x000099b8, 0xffff6a3c,
    0x00001988, 0xffffe898, 0x000001bc, 0xfffffe7c, 0x000001a0, 0x0000187c, 0x000097fc, 0x0003e84c, 0xffff6424,
    0xffffff4c, 0x00000248, 0xffffffec,
};

typedef int ARRAY3[3]; /* for short-block reordering */

/* optional pre-emphasis for high-frequency scale factor bands */
static const char preTab[22] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 3, 2, 0};

/* pow(2,-i/4) for i=0..3, Q31 format */
const int pow14[4] = {0x7fffffff, 0x6ba27e65, 0x5a82799a, 0x4c1bf829};

/* pow(2,-i/4) * pow(j,4/3) for i=0..3 j=0..15, Q25 format */
const int pow43_14[4][16] = {
    {
        0x00000000,
        0x10000000,
        0x285145f3,
        0x453a5cdb, /* Q28 */
        0x0cb2ff53,
        0x111989d6,
        0x15ce31c8,
        0x1ac7f203,
        0x20000000,
        0x257106b9,
        0x2b16b4a3,
        0x30ed74b4,
        0x36f23fa5,
        0x3d227bd3,
        0x437be656,
        0x49fc823c,
    },

    {
        0x00000000,
        0x0d744fcd,
        0x21e71f26,
        0x3a36abd9,
        0x0aadc084,
        0x0e610e6e,
        0x12560c1d,
        0x168523cf,
        0x1ae89f99,
        0x1f7c03a4,
        0x243bae49,
        0x29249c67,
        0x2e34420f,
        0x33686f85,
        0x38bf3dff,
        0x3e370182,
    },

    {
        0x00000000,
        0x0b504f33,
        0x1c823e07,
        0x30f39a55,
        0x08facd62,
        0x0c176319,
        0x0f6b3522,
        0x12efe2ad,
        0x16a09e66,
        0x1a79a317,
        0x1e77e301,
        0x2298d5b4,
        0x26da56fc,
        0x2b3a902a,
        0x2fb7e7e7,
        0x3450f650,
    },

    {
        0x00000000,
        0x09837f05,
        0x17f910d7,
        0x2929c7a9,
        0x078d0dfa,
        0x0a2ae661,
        0x0cf73154,
        0x0fec91cb,
        0x1306fe0a,
        0x16434a6c,
        0x199ee595,
        0x1d17ae3d,
        0x20abd76a,
        0x2459d551,
        0x28204fbb,
        0x2bfe1808,
    },
};

/* pow(j,4/3) for j=16..63, Q23 format */
const int pow43[] = {
    0x1428a2fa, 0x15db1bd6, 0x1796302c, 0x19598d85, 0x1b24e8bb, 0x1cf7fcfa, 0x1ed28af2, 0x20b4582a,
    0x229d2e6e, 0x248cdb55, 0x26832fda, 0x28800000, 0x2a832287, 0x2c8c70a8, 0x2e9bc5d8, 0x30b0ff99,
    0x32cbfd4a, 0x34eca001, 0x3712ca62, 0x393e6088, 0x3b6f47e0, 0x3da56717, 0x3fe0a5fc, 0x4220ed72,
    0x44662758, 0x46b03e7c, 0x48ff1e87, 0x4b52b3f3, 0x4daaebfd, 0x5007b497, 0x5268fc62, 0x54ceb29c,
    0x5738c721, 0x59a72a59, 0x5c19cd35, 0x5e90a129, 0x610b9821, 0x638aa47f, 0x660db90f, 0x6894c90b,
    0x6b1fc80c, 0x6daeaa0d, 0x70416360, 0x72d7e8b0, 0x75722ef9, 0x78102b85, 0x7ab1d3ec, 0x7d571e09,
};

/* sqrt(0.5) in Q31 format */
#define SQRTHALF 0x5a82799a

/*
 * Minimax polynomial approximation to pow(x, 4/3), over the range
 *  poly43lo: x = [0.5, 0.7071]
 *  poly43hi: x = [0.7071, 1.0]
 *
 * Relative error < 1E-7
 * Coefs are scaled by 4, 2, 1, 0.5, 0.25
 */
const unsigned int poly43lo[5] = {0x29a0bda9, 0xb02e4828, 0x5957aa1b, 0x236c498d, 0xff581859};
const unsigned int poly43hi[5] = {0x10852163, 0xd333f6a4, 0x46e9408b, 0x27c2cef0, 0xfef577b4};

/* pow(2, i*4/3) as exp and frac */
const int pow2exp[8] = {14, 13, 11, 10, 9, 7, 6, 5};

const int pow2frac[8] = {0x6597fa94, 0x50a28be6, 0x7fffffff, 0x6597fa94,
                         0x50a28be6, 0x7fffffff, 0x6597fa94, 0x50a28be6};

/**************************************************************************************
 * Function:    DequantBlock
 *
 * Description: Ken's highly-optimized, low memory dequantizer performing the
 *operation y = pow(x, 4.0/3.0) * pow(2, 25 - scale/4.0)
 *
 * Inputs:      input buffer of decode Huffman codewords (signed-magnitude)
 *              output buffer of same length (in-place (outbuf = inbuf) is
 *allowed) number of samples
 *
 * Outputs:     dequantized samples in Q25 format
 *
 * Return:      bitwise-OR of the unsigned outputs (for guard bit calculations)
 **************************************************************************************/
static int DequantBlock(int *inbuf, int *outbuf, int num, int scale) {
  int tab4[4];
  int scalef, scalei, shift;
  int sx, x, y;
  int mask = 0;
  const int *tab16;
  const unsigned int *coef;

  tab16 = pow43_14[scale & 0x3];
  scalef = pow14[scale & 0x3];
  scalei = MIN(scale >> 2, 31); /* smallest input scale = -47, so smallest scalei = -12 */

  /* cache first 4 values */
  shift = MIN(scalei + 3, 31);
  shift = MAX(shift, 0);
  tab4[0] = 0;
  tab4[1] = tab16[1] >> shift;
  tab4[2] = tab16[2] >> shift;
  tab4[3] = tab16[3] >> shift;

  do {
    sx = *inbuf++;
    x = sx & 0x7fffffff; /* sx = sign|mag */

    if (x < 4) {
      y = tab4[x];

    } else if (x < 16) {
      y = tab16[x];
      y = (scalei < 0) ? y << -scalei : y >> scalei;

    } else {
      if (x < 64) {
        y = pow43[x - 16];

        /* fractional scale */
        y = MULSHIFT32(y, scalef);
        shift = scalei - 3;

      } else {
        /* normalize to [0x40000000, 0x7fffffff] */
        x <<= 17;
        shift = 0;
        if (x < 0x08000000)
          x <<= 4, shift += 4;
        if (x < 0x20000000)
          x <<= 2, shift += 2;
        if (x < 0x40000000)
          x <<= 1, shift += 1;

        coef = (x < SQRTHALF) ? poly43lo : poly43hi;

        /* polynomial */
        y = coef[0];
        y = MULSHIFT32(y, x) + coef[1];
        y = MULSHIFT32(y, x) + coef[2];
        y = MULSHIFT32(y, x) + coef[3];
        y = MULSHIFT32(y, x) + coef[4];
        y = MULSHIFT32(y, pow2frac[shift]) << 3;

        /* fractional scale */
        y = MULSHIFT32(y, scalef);
        shift = scalei - pow2exp[shift];
      }

      /* integer scale */
      if (shift < 0) {
        shift = -shift;
        if (y > (0x7fffffff >> shift))
          y = 0x7fffffff; /* clip */
        else
          y <<= shift;
      } else {
        y >>= shift;
      }
    }

    /* sign and store */
    mask |= y;
    *outbuf++ = (sx < 0) ? -y : y;

  } while (--num);

  return mask;
}

/**************************************************************************************
 * Function:    DequantChannel
 *
 * Description: dequantize one granule, one channel worth of decoded Huffman
 *codewords
 *
 * Inputs:      sample buffer (decoded Huffman codewords), length = MAX_NSAMP
 *samples work buffer for reordering short-block, length = MAX_REORDER_SAMPS
 *                samples (3 * width of largest short-block critical band)
 *              non-zero bound for this channel/granule
 *              valid FrameHeader, SideInfoSub, ScaleFactorInfoSub, and
 *CriticalBandInfo structures for this channel/granule
 *
 * Outputs:     MAX_NSAMP dequantized samples in sampleBuf
 *              updated non-zero bound (indicating which samples are != 0 after
 *DQ) filled-in cbi structure indicating start and end critical bands
 *
 * Return:      minimum number of guard bits in dequantized sampleBuf
 *
 * Notes:       dequantized samples in Q(DQ_FRACBITS_OUT) format
 **************************************************************************************/
int DequantChannel(int *sampleBuf, int *workBuf, int *nonZeroBound, FrameHeader *fh, SideInfoSub *sis,
                   ScaleFactorInfoSub *sfis, CriticalBandInfo *cbi) {
  int i, j, w, cb;
  int cbStartL, cbEndL, cbStartS, cbEndS;
  int nSamps, nonZero, sfactMultiplier, gbMask;
  int globalGain, gainI;
  int cbMax[3];
  ARRAY3 *buf; /* short block reorder */

  /* set default start/end points for short/long blocks - will update with
   * non-zero cb info */
  if (sis->blockType == 2) {
    cbStartL = 0;
    if (sis->mixedBlock) {
      cbEndL = (fh->ver == MPEG1 ? 8 : 6);
      cbStartS = 3;
    } else {
      cbEndL = 0;
      cbStartS = 0;
    }
    cbEndS = 13;
  } else {
    /* long block */
    cbStartL = 0;
    cbEndL = 22;
    cbStartS = 13;
    cbEndS = 13;
  }
  cbMax[2] = cbMax[1] = cbMax[0] = 0;
  gbMask = 0;
  i = 0;

  /* sfactScale = 0 --> quantizer step size = 2
   * sfactScale = 1 --> quantizer step size = sqrt(2)
   *   so sfactMultiplier = 2 or 4 (jump through globalGain by powers of 2 or
   * sqrt(2))
   */
  sfactMultiplier = 2 * (sis->sfactScale + 1);

  /* offset globalGain by -2 if midSide enabled, for 1/sqrt(2) used in
   * MidSideProc() (DequantBlock() does 0.25 * gainI so knocking it down by two
   * is the same as dividing every sample by sqrt(2) = multiplying by 2^-.5)
   */
  globalGain = sis->globalGain;
  if (fh->modeExt >> 1)
    globalGain -= 2;
  globalGain += IMDCT_SCALE; /* scale everything by sqrt(2), for fast IMDCT36 */

  /* long blocks */
  for (cb = 0; cb < cbEndL; cb++) {
    nonZero = 0;
    nSamps = fh->sfBand->l[cb + 1] - fh->sfBand->l[cb];
    gainI = 210 - globalGain + sfactMultiplier * (sfis->l[cb] + (sis->preFlag ? (int) preTab[cb] : 0));

    nonZero |= DequantBlock(sampleBuf + i, sampleBuf + i, nSamps, gainI);
    i += nSamps;

    /* update highest non-zero critical band */
    if (nonZero)
      cbMax[0] = cb;
    gbMask |= nonZero;

    if (i >= *nonZeroBound)
      break;
  }

  /* set cbi (Type, EndS[], EndSMax will be overwritten if we proceed to do
   * short blocks) */
  cbi->cbType = 0; /* long only */
  cbi->cbEndL = cbMax[0];
  cbi->cbEndS[0] = cbi->cbEndS[1] = cbi->cbEndS[2] = 0;
  cbi->cbEndSMax = 0;

  /* early exit if no short blocks */
  if (cbStartS >= 12)
    return CLZ(gbMask) - 1;

  /* short blocks */
  cbMax[2] = cbMax[1] = cbMax[0] = cbStartS;
  for (cb = cbStartS; cb < cbEndS; cb++) {
    nSamps = fh->sfBand->s[cb + 1] - fh->sfBand->s[cb];
    for (w = 0; w < 3; w++) {
      nonZero = 0;
      gainI = 210 - globalGain + 8 * sis->subBlockGain[w] + sfactMultiplier * (sfis->s[cb][w]);

      nonZero |= DequantBlock(sampleBuf + i + nSamps * w, workBuf + nSamps * w, nSamps, gainI);

      /* update highest non-zero critical band */
      if (nonZero)
        cbMax[w] = cb;
      gbMask |= nonZero;
    }

    /* reorder blocks */
    buf = (ARRAY3 *) (sampleBuf + i);
    i += 3 * nSamps;
    for (j = 0; j < nSamps; j++) {
      buf[j][0] = workBuf[0 * nSamps + j];
      buf[j][1] = workBuf[1 * nSamps + j];
      buf[j][2] = workBuf[2 * nSamps + j];
    }

    ASSERT(3 * nSamps <= MAX_REORDER_SAMPS);

    if (i >= *nonZeroBound)
      break;
  }

  /* i = last non-zero INPUT sample processed, which corresponds to highest
   * possible non-zero OUTPUT sample (after reorder) however, the original nzb
   * is no longer necessarily true for each cb, buf[][] is updated with 3*nSamps
   * samples (i increases 3*nSamps each time) (buf[j + 1][0] = 3 (input) samples
   * ahead of buf[j][0]) so update nonZeroBound to i
   */
  *nonZeroBound = i;

  ASSERT(*nonZeroBound <= MAX_NSAMP);

  cbi->cbType = (sis->mixedBlock ? 2 : 1); /* 2 = mixed short/long, 1 = short only */

  cbi->cbEndS[0] = cbMax[0];
  cbi->cbEndS[1] = cbMax[1];
  cbi->cbEndS[2] = cbMax[2];

  cbi->cbEndSMax = cbMax[0];
  cbi->cbEndSMax = MAX(cbi->cbEndSMax, cbMax[1]);
  cbi->cbEndSMax = MAX(cbi->cbEndSMax, cbMax[2]);

  return CLZ(gbMask) - 1;
}

/* input to Polyphase = Q(DQ_FRACBITS_OUT-2), gain 2 bits in convolution
 *  we also have the implicit bias of 2^15 to add back, so net fraction bits =
 *    DQ_FRACBITS_OUT - 2 - 2 - 15
 *  (see comment on Dequantize() for more info)
 */
#define DEF_NFRACBITS (DQ_FRACBITS_OUT - 2 - 2 - 15)
#define CSHIFT \
  12 /* coefficients have 12 leading sign bits for early-terminating \
        mulitplies */

static __inline short ClipToShort(int x, int fracBits) {
  int sign;

  /* assumes you've already rounded (x += (1 << (fracBits-1))) */
  x >>= fracBits;

  /* Ken's trick: clips to [-32768, 32767] */
  sign = x >> 31;
  if (sign != (x >> 15))
    x = sign ^ ((1 << 15) - 1);

  return (short) x;
}

#define MC0M(x) \
  { \
    c1 = *coef; \
    coef++; \
    c2 = *coef; \
    coef++; \
    vLo = *(vb1 + (x)); \
    vHi = *(vb1 + (23 - (x))); \
    sum1L = MADD64(sum1L, vLo, c1); \
    sum1L = MADD64(sum1L, vHi, -c2); \
  }

#define MC1M(x) \
  { \
    c1 = *coef; \
    coef++; \
    vLo = *(vb1 + (x)); \
    sum1L = MADD64(sum1L, vLo, c1); \
  }

#define MC2M(x) \
  { \
    c1 = *coef; \
    coef++; \
    c2 = *coef; \
    coef++; \
    vLo = *(vb1 + (x)); \
    vHi = *(vb1 + (23 - (x))); \
    sum1L = MADD64(sum1L, vLo, c1); \
    sum2L = MADD64(sum2L, vLo, c2); \
    sum1L = MADD64(sum1L, vHi, -c2); \
    sum2L = MADD64(sum2L, vHi, c1); \
  }

/**************************************************************************************
 * Function:    PolyphaseMono
 *
 * Description: filter one subband and produce 32 output PCM samples for one
 *channel
 *
 * Inputs:      pointer to PCM output buffer
 *              number of "extra shifts" (vbuf format = Q(DQ_FRACBITS_OUT-2))
 *              pointer to start of vbuf (preserved from last call)
 *              start of filter coefficient table (in proper, shuffled order)
 *              no minimum number of guard bits is required for input vbuf
 *                (see additional scaling comments below)
 *
 * Outputs:     32 samples of one channel of decoded PCM data, (i.e. Q16.0)
 *
 * Return:      none
 *
 * TODO:        add 32-bit version for platforms where 64-bit mul-acc is not
 *supported (note max filter gain - see polyCoef[] comments)
 **************************************************************************************/
void PolyphaseMono(short *pcm, int *vbuf, const uint32_t *coefBase) {
  int i;
  const uint32_t *coef;
  int *vb1;
  int vLo, vHi, c1, c2;
  Word64 sum1L, sum2L, rndVal;

  rndVal = (Word64) (1 << (DEF_NFRACBITS - 1 + (32 - CSHIFT)));

  /* special case, output sample 0 */
  coef = coefBase;
  vb1 = vbuf;
  sum1L = rndVal;

  MC0M(0)
  MC0M(1)
  MC0M(2)
  MC0M(3)
  MC0M(4)
  MC0M(5)
  MC0M(6)
  MC0M(7)

  *(pcm + 0) = ClipToShort((int) SAR64(sum1L, (32 - CSHIFT)), DEF_NFRACBITS);

  /* special case, output sample 16 */
  coef = coefBase + 256;
  vb1 = vbuf + 64 * 16;
  sum1L = rndVal;

  MC1M(0)
  MC1M(1)
  MC1M(2)
  MC1M(3)
  MC1M(4)
  MC1M(5)
  MC1M(6)
  MC1M(7)

  *(pcm + 16) = ClipToShort((int) SAR64(sum1L, (32 - CSHIFT)), DEF_NFRACBITS);

  /* main convolution loop: sum1L = samples 1, 2, 3, ... 15   sum2L = samples
   * 31, 30, ... 17 */
  coef = coefBase + 16;
  vb1 = vbuf + 64;
  pcm++;

  /* right now, the compiler creates bad asm from this... */
  for (i = 15; i > 0; i--) {
    sum1L = sum2L = rndVal;

    MC2M(0)
    MC2M(1)
    MC2M(2)
    MC2M(3)
    MC2M(4)
    MC2M(5)
    MC2M(6)
    MC2M(7)

    vb1 += 64;
    *(pcm) = ClipToShort((int) SAR64(sum1L, (32 - CSHIFT)), DEF_NFRACBITS);
    *(pcm + 2 * i) = ClipToShort((int) SAR64(sum2L, (32 - CSHIFT)), DEF_NFRACBITS);
    pcm++;
  }
}

#define MC0S(x) \
  { \
    c1 = *coef; \
    coef++; \
    c2 = *coef; \
    coef++; \
    vLo = *(vb1 + (x)); \
    vHi = *(vb1 + (23 - (x))); \
    sum1L = MADD64(sum1L, vLo, c1); \
    sum1L = MADD64(sum1L, vHi, -c2); \
    vLo = *(vb1 + 32 + (x)); \
    vHi = *(vb1 + 32 + (23 - (x))); \
    sum1R = MADD64(sum1R, vLo, c1); \
    sum1R = MADD64(sum1R, vHi, -c2); \
  }

#define MC1S(x) \
  { \
    c1 = *coef; \
    coef++; \
    vLo = *(vb1 + (x)); \
    sum1L = MADD64(sum1L, vLo, c1); \
    vLo = *(vb1 + 32 + (x)); \
    sum1R = MADD64(sum1R, vLo, c1); \
  }

#define MC2S(x) \
  { \
    c1 = *coef; \
    coef++; \
    c2 = *coef; \
    coef++; \
    vLo = *(vb1 + (x)); \
    vHi = *(vb1 + (23 - (x))); \
    sum1L = MADD64(sum1L, vLo, c1); \
    sum2L = MADD64(sum2L, vLo, c2); \
    sum1L = MADD64(sum1L, vHi, -c2); \
    sum2L = MADD64(sum2L, vHi, c1); \
    vLo = *(vb1 + 32 + (x)); \
    vHi = *(vb1 + 32 + (23 - (x))); \
    sum1R = MADD64(sum1R, vLo, c1); \
    sum2R = MADD64(sum2R, vLo, c2); \
    sum1R = MADD64(sum1R, vHi, -c2); \
    sum2R = MADD64(sum2R, vHi, c1); \
  }

/**************************************************************************************
 * Function:    PolyphaseStereo
 *
 * Description: filter one subband and produce 32 output PCM samples for each
 *channel
 *
 * Inputs:      pointer to PCM output buffer
 *              number of "extra shifts" (vbuf format = Q(DQ_FRACBITS_OUT-2))
 *              pointer to start of vbuf (preserved from last call)
 *              start of filter coefficient table (in proper, shuffled order)
 *              no minimum number of guard bits is required for input vbuf
 *                (see additional scaling comments below)
 *
 * Outputs:     32 samples of two channels of decoded PCM data, (i.e. Q16.0)
 *
 * Return:      none
 *
 * Notes:       interleaves PCM samples LRLRLR...
 *
 * TODO:        add 32-bit version for platforms where 64-bit mul-acc is not
 *supported
 **************************************************************************************/
void PolyphaseStereo(short *pcm, int *vbuf, const uint32_t *coefBase) {
  int i;
  const uint32_t *coef;
  int *vb1;
  int vLo, vHi, c1, c2;
  Word64 sum1L, sum2L, sum1R, sum2R, rndVal;

  rndVal = (Word64) (1 << (DEF_NFRACBITS - 1 + (32 - CSHIFT)));

  /* special case, output sample 0 */
  coef = coefBase;
  vb1 = vbuf;
  sum1L = sum1R = rndVal;

  MC0S(0)
  MC0S(1)
  MC0S(2)
  MC0S(3)
  MC0S(4)
  MC0S(5)
  MC0S(6)
  MC0S(7)

  *(pcm + 0) = ClipToShort((int) SAR64(sum1L, (32 - CSHIFT)), DEF_NFRACBITS);
  *(pcm + 1) = ClipToShort((int) SAR64(sum1R, (32 - CSHIFT)), DEF_NFRACBITS);

  /* special case, output sample 16 */
  coef = coefBase + 256;
  vb1 = vbuf + 64 * 16;
  sum1L = sum1R = rndVal;

  MC1S(0)
  MC1S(1)
  MC1S(2)
  MC1S(3)
  MC1S(4)
  MC1S(5)
  MC1S(6)
  MC1S(7)

  *(pcm + 2 * 16 + 0) = ClipToShort((int) SAR64(sum1L, (32 - CSHIFT)), DEF_NFRACBITS);
  *(pcm + 2 * 16 + 1) = ClipToShort((int) SAR64(sum1R, (32 - CSHIFT)), DEF_NFRACBITS);

  /* main convolution loop: sum1L = samples 1, 2, 3, ... 15   sum2L = samples
   * 31, 30, ... 17 */
  coef = coefBase + 16;
  vb1 = vbuf + 64;
  pcm += 2;

  /* right now, the compiler creates bad asm from this... */
  for (i = 15; i > 0; i--) {
    sum1L = sum2L = rndVal;
    sum1R = sum2R = rndVal;

    MC2S(0)
    MC2S(1)
    MC2S(2)
    MC2S(3)
    MC2S(4)
    MC2S(5)
    MC2S(6)
    MC2S(7)

    vb1 += 64;
    *(pcm + 0) = ClipToShort((int) SAR64(sum1L, (32 - CSHIFT)), DEF_NFRACBITS);
    *(pcm + 1) = ClipToShort((int) SAR64(sum1R, (32 - CSHIFT)), DEF_NFRACBITS);
    *(pcm + 2 * 2 * i + 0) = ClipToShort((int) SAR64(sum2L, (32 - CSHIFT)), DEF_NFRACBITS);
    *(pcm + 2 * 2 * i + 1) = ClipToShort((int) SAR64(sum2R, (32 - CSHIFT)), DEF_NFRACBITS);
    pcm += 2;
  }
}

/**************************************************************************************
 * Function:    Subband
 *
 * Description: do subband transform on all the blocks in one granule, all
 *channels
 *
 * Inputs:      filled MP3DecInfo structure, after calling IMDCT for all
 *channels vbuf[ch] and vindex[ch] must be preserved between calls
 *
 * Outputs:     decoded PCM data, interleaved LRLRLR... if stereo
 *
 * Return:      0 on success,  -1 if null input pointers
 **************************************************************************************/
int Subband(MP3DecInfo *mp3DecInfo, short *pcmBuf) {
  int b;
  HuffmanInfo *hi;
  IMDCTInfo *mi;
  SubbandInfo *sbi;

  /* validate pointers */
  if (!mp3DecInfo || !mp3DecInfo->HuffmanInfoPS || !mp3DecInfo->IMDCTInfoPS || !mp3DecInfo->SubbandInfoPS)
    return -1;

  hi = (HuffmanInfo *) mp3DecInfo->HuffmanInfoPS;
  mi = (IMDCTInfo *) (mp3DecInfo->IMDCTInfoPS);
  sbi = (SubbandInfo *) (mp3DecInfo->SubbandInfoPS);

  if (mp3DecInfo->nChans == 2) {
    /* stereo */
    for (b = 0; b < BLOCK_SIZE; b++) {
      FDCT32(mi->outBuf[0][b], sbi->vbuf + 0 * 32, sbi->vindex, (b & 0x01), mi->gb[0]);
      FDCT32(mi->outBuf[1][b], sbi->vbuf + 1 * 32, sbi->vindex, (b & 0x01), mi->gb[1]);
      PolyphaseStereo(pcmBuf, sbi->vbuf + sbi->vindex + VBUF_LENGTH * (b & 0x01), polyCoef);
      sbi->vindex = (sbi->vindex - (b & 0x01)) & 7;
      pcmBuf += (2 * NBANDS);
    }
  } else {
    /* mono */
    for (b = 0; b < BLOCK_SIZE; b++) {
      FDCT32(mi->outBuf[0][b], sbi->vbuf + 0 * 32, sbi->vindex, (b & 0x01), mi->gb[0]);
      PolyphaseMono(pcmBuf, sbi->vbuf + sbi->vindex + VBUF_LENGTH * (b & 0x01), polyCoef);
      sbi->vindex = (sbi->vindex - (b & 0x01)) & 7;
      pcmBuf += NBANDS;
    }
  }

  return 0;
}

/**************************************************************************************
 * Function:    MidSideProc
 *
 * Description: sum-difference stereo reconstruction
 *
 * Inputs:      vector x with dequantized samples from left and right channels
 *              number of non-zero samples (MAX of left and right)
 *              assume 1 guard bit in input
 *              guard bit mask (left and right channels)
 *
 * Outputs:     updated sample vector x
 *              updated guard bit mask
 *
 * Return:      none
 *
 * Notes:       assume at least 1 GB in input
 **************************************************************************************/
void MidSideProc(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, int mOut[2]) {
  int i, xr, xl, mOutL, mOutR;

  /* L = (M+S)/sqrt(2), R = (M-S)/sqrt(2)
   * NOTE: 1/sqrt(2) done in DequantChannel() - see comments there
   */
  mOutL = mOutR = 0;
  for (i = 0; i < nSamps; i++) {
    xl = x[0][i];
    xr = x[1][i];
    x[0][i] = xl + xr;
    x[1][i] = xl - xr;
    mOutL |= FASTABS(x[0][i]);
    mOutR |= FASTABS(x[1][i]);
  }
  mOut[0] |= mOutL;
  mOut[1] |= mOutR;
}

/**************************************************************************************
 * Function:    IntensityProcMPEG1
 *
 * Description: intensity stereo processing for MPEG1
 *
 * Inputs:      vector x with dequantized samples from left and right channels
 *              number of non-zero samples in left channel
 *              valid FrameHeader struct
 *              two each of ScaleFactorInfoSub, CriticalBandInfo structs (both
 *channels) flags indicating midSide on/off, mixedBlock on/off guard bit mask
 *(left and right channels)
 *
 * Outputs:     updated sample vector x
 *              updated guard bit mask
 *
 * Return:      none
 *
 * Notes:       assume at least 1 GB in input
 *
 * TODO:        combine MPEG1/2 into one function (maybe)
 *              make sure all the mixed-block and IIP logic is right
 **************************************************************************************/
void IntensityProcMPEG1(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, FrameHeader *fh, ScaleFactorInfoSub *sfis,
                        CriticalBandInfo *cbi, int midSideFlag, int mixFlag, int mOut[2]) {
  int i = 0, j = 0, n = 0, cb = 0, w = 0;
  int sampsLeft, isf, mOutL, mOutR, xl, xr;
  int fl, fr, fls[3], frs[3];
  int cbStartL = 0, cbStartS = 0, cbEndL = 0, cbEndS = 0;
  int *isfTab;

  /* NOTE - this works fine for mixed blocks, as long as the switch point starts
   * in the short block section (i.e. on or after sample 36 = sfBand->l[8] =
   * 3*sfBand->s[3] is this a safe assumption?
   * TODO - intensity + mixed not quite right (diff = 11 on he_mode)
   *  figure out correct implementation (spec ambiguous about when to do short
   * block reorder)
   */
  if (cbi[1].cbType == 0) {
    /* long block */
    cbStartL = cbi[1].cbEndL + 1;
    cbEndL = cbi[0].cbEndL + 1;
    cbStartS = cbEndS = 0;
    i = fh->sfBand->l[cbStartL];
  } else if (cbi[1].cbType == 1 || cbi[1].cbType == 2) {
    /* short or mixed block */
    cbStartS = cbi[1].cbEndSMax + 1;
    cbEndS = cbi[0].cbEndSMax + 1;
    cbStartL = cbEndL = 0;
    i = 3 * fh->sfBand->s[cbStartS];
  }

  sampsLeft = nSamps - i; /* process to length of left */
  isfTab = (int *) ISFMpeg1[midSideFlag];
  mOutL = mOutR = 0;

  /* long blocks */
  for (cb = cbStartL; cb < cbEndL && sampsLeft > 0; cb++) {
    isf = sfis->l[cb];
    if (isf == 7) {
      fl = ISFIIP[midSideFlag][0];
      fr = ISFIIP[midSideFlag][1];
    } else {
      fl = isfTab[isf];
      fr = isfTab[6] - isfTab[isf];
    }

    n = fh->sfBand->l[cb + 1] - fh->sfBand->l[cb];
    for (j = 0; j < n && sampsLeft > 0; j++, i++) {
      xr = MULSHIFT32(fr, x[0][i]) << 2;
      x[1][i] = xr;
      mOutR |= FASTABS(xr);
      xl = MULSHIFT32(fl, x[0][i]) << 2;
      x[0][i] = xl;
      mOutL |= FASTABS(xl);
      sampsLeft--;
    }
  }

  /* short blocks */
  for (cb = cbStartS; cb < cbEndS && sampsLeft >= 3; cb++) {
    for (w = 0; w < 3; w++) {
      isf = sfis->s[cb][w];
      if (isf == 7) {
        fls[w] = ISFIIP[midSideFlag][0];
        frs[w] = ISFIIP[midSideFlag][1];
      } else {
        fls[w] = isfTab[isf];
        frs[w] = isfTab[6] - isfTab[isf];
      }
    }

    n = fh->sfBand->s[cb + 1] - fh->sfBand->s[cb];
    for (j = 0; j < n && sampsLeft >= 3; j++, i += 3) {
      xr = MULSHIFT32(frs[0], x[0][i + 0]) << 2;
      x[1][i + 0] = xr;
      mOutR |= FASTABS(xr);
      xl = MULSHIFT32(fls[0], x[0][i + 0]) << 2;
      x[0][i + 0] = xl;
      mOutL |= FASTABS(xl);
      xr = MULSHIFT32(frs[1], x[0][i + 1]) << 2;
      x[1][i + 1] = xr;
      mOutR |= FASTABS(xr);
      xl = MULSHIFT32(fls[1], x[0][i + 1]) << 2;
      x[0][i + 1] = xl;
      mOutL |= FASTABS(xl);
      xr = MULSHIFT32(frs[2], x[0][i + 2]) << 2;
      x[1][i + 2] = xr;
      mOutR |= FASTABS(xr);
      xl = MULSHIFT32(fls[2], x[0][i + 2]) << 2;
      x[0][i + 2] = xl;
      mOutL |= FASTABS(xl);
      sampsLeft -= 3;
    }
  }
  mOut[0] = mOutL;
  mOut[1] = mOutR;

  return;
}

/**************************************************************************************
 * Function:    IntensityProcMPEG2
 *
 * Description: intensity stereo processing for MPEG2
 *
 * Inputs:      vector x with dequantized samples from left and right channels
 *              number of non-zero samples in left channel
 *              valid FrameHeader struct
 *              two each of ScaleFactorInfoSub, CriticalBandInfo structs (both
 *channels) ScaleFactorJS struct with joint stereo info from UnpackSFMPEG2()
 *              flags indicating midSide on/off, mixedBlock on/off
 *              guard bit mask (left and right channels)
 *
 * Outputs:     updated sample vector x
 *              updated guard bit mask
 *
 * Return:      none
 *
 * Notes:       assume at least 1 GB in input
 *
 * TODO:        combine MPEG1/2 into one function (maybe)
 *              make sure all the mixed-block and IIP logic is right
 *                probably redo IIP logic to be simpler
 **************************************************************************************/
void IntensityProcMPEG2(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, FrameHeader *fh, ScaleFactorInfoSub *sfis,
                        CriticalBandInfo *cbi, ScaleFactorJS *sfjs, int midSideFlag, int mixFlag, int mOut[2]) {
  int i, j, k, n, r, cb, w;
  int fl, fr, mOutL, mOutR, xl, xr;
  int sampsLeft;
  int isf, sfIdx, tmp, il[23];
  int *isfTab;
  int cbStartL, cbStartS, cbEndL, cbEndS;

  isfTab = (int *) ISFMpeg2[sfjs->intensityScale][midSideFlag];
  mOutL = mOutR = 0;

  /* fill buffer with illegal intensity positions (depending on slen) */
  for (k = r = 0; r < 4; r++) {
    tmp = (1 << sfjs->slen[r]) - 1;
    for (j = 0; j < sfjs->nr[r]; j++, k++)
      il[k] = tmp;
  }

  if (cbi[1].cbType == 0) {
    /* long blocks */
    il[21] = il[22] = 1;
    cbStartL = cbi[1].cbEndL + 1; /* start at end of right */
    cbEndL = cbi[0].cbEndL + 1;   /* process to end of left */
    i = fh->sfBand->l[cbStartL];
    sampsLeft = nSamps - i;

    for (cb = cbStartL; cb < cbEndL; cb++) {
      sfIdx = sfis->l[cb];
      if (sfIdx == il[cb]) {
        fl = ISFIIP[midSideFlag][0];
        fr = ISFIIP[midSideFlag][1];
      } else {
        isf = (sfis->l[cb] + 1) >> 1;
        fl = isfTab[(sfIdx & 0x01 ? isf : 0)];
        fr = isfTab[(sfIdx & 0x01 ? 0 : isf)];
      }
      n = MIN(fh->sfBand->l[cb + 1] - fh->sfBand->l[cb], sampsLeft);

      for (j = 0; j < n; j++, i++) {
        xr = MULSHIFT32(fr, x[0][i]) << 2;
        x[1][i] = xr;
        mOutR |= FASTABS(xr);
        xl = MULSHIFT32(fl, x[0][i]) << 2;
        x[0][i] = xl;
        mOutL |= FASTABS(xl);
      }

      /* early exit once we've used all the non-zero samples */
      sampsLeft -= n;
      if (sampsLeft == 0)
        break;
    }
  } else {
    /* short or mixed blocks */
    il[12] = 1;

    for (w = 0; w < 3; w++) {
      cbStartS = cbi[1].cbEndS[w] + 1; /* start at end of right */
      cbEndS = cbi[0].cbEndS[w] + 1;   /* process to end of left */
      i = 3 * fh->sfBand->s[cbStartS] + w;

      /* skip through sample array by 3, so early-exit logic would be more
       * tricky */
      for (cb = cbStartS; cb < cbEndS; cb++) {
        sfIdx = sfis->s[cb][w];
        if (sfIdx == il[cb]) {
          fl = ISFIIP[midSideFlag][0];
          fr = ISFIIP[midSideFlag][1];
        } else {
          isf = (sfis->s[cb][w] + 1) >> 1;
          fl = isfTab[(sfIdx & 0x01 ? isf : 0)];
          fr = isfTab[(sfIdx & 0x01 ? 0 : isf)];
        }
        n = fh->sfBand->s[cb + 1] - fh->sfBand->s[cb];

        for (j = 0; j < n; j++, i += 3) {
          xr = MULSHIFT32(fr, x[0][i]) << 2;
          x[1][i] = xr;
          mOutR |= FASTABS(xr);
          xl = MULSHIFT32(fl, x[0][i]) << 2;
          x[0][i] = xl;
          mOutL |= FASTABS(xl);
        }
      }
    }
  }
  mOut[0] = mOutL;
  mOut[1] = mOutR;

  return;
}

/* scale factor lengths (num bits) */
static const char SFLenTab[16][2] = {
    {0, 0}, {0, 1}, {0, 2}, {0, 3}, {3, 0}, {1, 1}, {1, 2}, {1, 3},
    {2, 1}, {2, 2}, {2, 3}, {3, 1}, {3, 2}, {3, 3}, {4, 2}, {4, 3},
};

/**************************************************************************************
 * Function:    UnpackSFMPEG1
 *
 * Description: unpack MPEG 1 scalefactors from bitstream
 *
 * Inputs:      BitStreamInfo, SideInfoSub, ScaleFactorInfoSub structs for this
 *                granule/channel
 *              vector of scfsi flags from side info, length = 4 (MAX_SCFBD)
 *              index of current granule
 *              ScaleFactorInfoSub from granule 0 (for granule 1, if scfsi[i] is
 *set, then we just replicate the scale factors from granule 0 in the i'th set
 *of scalefactor bands)
 *
 * Outputs:     updated BitStreamInfo struct
 *              scalefactors in sfis (short and/or long arrays, as appropriate)
 *
 * Return:      none
 *
 * Notes:       set order of short blocks to s[band][window] instead of
 *s[window][band] so that we index through consectutive memory locations when
 *unpacking (make sure dequantizer follows same convention) Illegal Intensity
 *Position = 7 (always) for MPEG1 scale factors
 **************************************************************************************/
static void UnpackSFMPEG1(BitStreamInfo *bsi, SideInfoSub *sis, ScaleFactorInfoSub *sfis, int *scfsi, int gr,
                          ScaleFactorInfoSub *sfisGr0) {
  int sfb;
  int slen0, slen1;

  /* these can be 0, so make sure GetBits(bsi, 0) returns 0 (no >> 32 or
   * anything) */
  slen0 = (int) SFLenTab[sis->sfCompress][0];
  slen1 = (int) SFLenTab[sis->sfCompress][1];

  if (sis->blockType == 2) {
    /* short block, type 2 (implies winSwitchFlag == 1) */
    if (sis->mixedBlock) {
      /* do long block portion */
      for (sfb = 0; sfb < 8; sfb++)
        sfis->l[sfb] = (char) GetBits(bsi, slen0);
      sfb = 3;
    } else {
      /* all short blocks */
      sfb = 0;
    }

    for (; sfb < 6; sfb++) {
      sfis->s[sfb][0] = (char) GetBits(bsi, slen0);
      sfis->s[sfb][1] = (char) GetBits(bsi, slen0);
      sfis->s[sfb][2] = (char) GetBits(bsi, slen0);
    }

    for (; sfb < 12; sfb++) {
      sfis->s[sfb][0] = (char) GetBits(bsi, slen1);
      sfis->s[sfb][1] = (char) GetBits(bsi, slen1);
      sfis->s[sfb][2] = (char) GetBits(bsi, slen1);
    }

    /* last sf band not transmitted */
    sfis->s[12][0] = sfis->s[12][1] = sfis->s[12][2] = 0;
  } else {
    /* long blocks, type 0, 1, or 3 */
    if (gr == 0) {
      /* first granule */
      for (sfb = 0; sfb < 11; sfb++)
        sfis->l[sfb] = (char) GetBits(bsi, slen0);
      for (sfb = 11; sfb < 21; sfb++)
        sfis->l[sfb] = (char) GetBits(bsi, slen1);
      return;
    } else {
      /* second granule
       * scfsi: 0 = different scalefactors for each granule, 1 = copy sf's from
       * granule 0 into granule 1 for block type == 2, scfsi is always 0
       */
      sfb = 0;
      if (scfsi[0])
        for (; sfb < 6; sfb++)
          sfis->l[sfb] = sfisGr0->l[sfb];
      else
        for (; sfb < 6; sfb++)
          sfis->l[sfb] = (char) GetBits(bsi, slen0);
      if (scfsi[1])
        for (; sfb < 11; sfb++)
          sfis->l[sfb] = sfisGr0->l[sfb];
      else
        for (; sfb < 11; sfb++)
          sfis->l[sfb] = (char) GetBits(bsi, slen0);
      if (scfsi[2])
        for (; sfb < 16; sfb++)
          sfis->l[sfb] = sfisGr0->l[sfb];
      else
        for (; sfb < 16; sfb++)
          sfis->l[sfb] = (char) GetBits(bsi, slen1);
      if (scfsi[3])
        for (; sfb < 21; sfb++)
          sfis->l[sfb] = sfisGr0->l[sfb];
      else
        for (; sfb < 21; sfb++)
          sfis->l[sfb] = (char) GetBits(bsi, slen1);
    }
    /* last sf band not transmitted */
    sfis->l[21] = 0;
    sfis->l[22] = 0;
  }
}

/* NRTab[size + 3*is_right][block type][partition]
 *   block type index: 0 = (bt0,bt1,bt3), 1 = bt2 non-mixed, 2 = bt2 mixed
 *   partition: scale factor groups (sfb1 through sfb4)
 * for block type = 2 (mixed or non-mixed) / by 3 is rolled into this table
 *   (for 3 short blocks per long block)
 * see 2.4.3.2 in MPEG 2 (low sample rate) spec
 * stuff rolled into this table:
 *   NRTab[x][1][y]   --> (NRTab[x][1][y])   / 3
 *   NRTab[x][2][>=1] --> (NRTab[x][2][>=1]) / 3  (first partition is long
 * block)
 */
static const char NRTab[6][3][4] = {
    /* non-intensity stereo */
    {
        {6, 5, 5, 5},
        {3, 3, 3, 3}, /* includes / 3 */
        {6, 3, 3, 3}, /* includes / 3 except for first entry */
    },
    {
        {6, 5, 7, 3},
        {3, 3, 4, 2},
        {6, 3, 4, 2},
    },
    {
        {11, 10, 0, 0}, {6, 6, 0, 0}, {6, 3, 6, 0}, /* spec = [15,18,0,0], but 15 = 6L + 9S, so move 9/3=3 into col 1,
                                                       18/3=6 into col 2 and adj. slen[1,2] below */
    },
    /* intensity stereo, right chan */
    {
        {7, 7, 7, 0},
        {4, 4, 4, 0},
        {6, 5, 4, 0},
    },
    {
        {6, 6, 6, 3},
        {4, 3, 3, 2},
        {6, 4, 3, 2},
    },
    {
        {8, 8, 5, 0},
        {5, 4, 3, 0},
        {6, 6, 3, 0},
    }};

/**************************************************************************************
 * Function:    UnpackSFMPEG2
 *
 * Description: unpack MPEG 2 scalefactors from bitstream
 *
 * Inputs:      BitStreamInfo, SideInfoSub, ScaleFactorInfoSub structs for this
 *                granule/channel
 *              index of current granule and channel
 *              ScaleFactorInfoSub from this granule
 *              modeExt field from frame header, to tell whether intensity
 *stereo is on ScaleFactorJS struct for storing IIP info used in Dequant()
 *
 * Outputs:     updated BitStreamInfo struct
 *              scalefactors in sfis (short and/or long arrays, as appropriate)
 *              updated intensityScale and preFlag flags
 *
 * Return:      none
 *
 * Notes:       Illegal Intensity Position = (2^slen) - 1 for MPEG2 scale
 *factors
 *
 * TODO:        optimize the / and % stuff (only do one divide, get modulo x
 *                with (x / m) * m, etc.)
 **************************************************************************************/
static void UnpackSFMPEG2(BitStreamInfo *bsi, SideInfoSub *sis, ScaleFactorInfoSub *sfis, int gr, int ch, int modeExt,
                          ScaleFactorJS *sfjs) {
  int i, sfb, sfcIdx, btIdx, nrIdx, iipTest;
  int slen[4], nr[4];
  int sfCompress, preFlag, intensityScale;

  sfCompress = sis->sfCompress;
  preFlag = 0;
  intensityScale = 0;

  /* stereo mode bits (1 = on): bit 1 = mid-side on/off, bit 0 = intensity
   * on/off */
  if (!((modeExt & 0x01) && (ch == 1))) {
    /* in other words: if ((modeExt & 0x01) == 0 || ch == 0) */
    if (sfCompress < 400) {
      /* max slen = floor[(399/16) / 5] = 4 */
      slen[0] = (sfCompress >> 4) / 5;
      slen[1] = (sfCompress >> 4) % 5;
      slen[2] = (sfCompress & 0x0f) >> 2;
      slen[3] = (sfCompress & 0x03);
      sfcIdx = 0;
    } else if (sfCompress < 500) {
      /* max slen = floor[(99/4) / 5] = 4 */
      sfCompress -= 400;
      slen[0] = (sfCompress >> 2) / 5;
      slen[1] = (sfCompress >> 2) % 5;
      slen[2] = (sfCompress & 0x03);
      slen[3] = 0;
      sfcIdx = 1;
    } else {
      /* max slen = floor[11/3] = 3 (sfCompress = 9 bits in MPEG2) */
      sfCompress -= 500;
      slen[0] = sfCompress / 3;
      slen[1] = sfCompress % 3;
      slen[2] = slen[3] = 0;
      if (sis->mixedBlock) {
        /* adjust for long/short mix logic (see comment above in NRTab[]
         * definition) */
        slen[2] = slen[1];
        slen[1] = slen[0];
      }
      preFlag = 1;
      sfcIdx = 2;
    }
  } else {
    /* intensity stereo ch = 1 (right) */
    intensityScale = sfCompress & 0x01;
    sfCompress >>= 1;
    if (sfCompress < 180) {
      /* max slen = floor[35/6] = 5 (from mod 36) */
      slen[0] = (sfCompress / 36);
      slen[1] = (sfCompress % 36) / 6;
      slen[2] = (sfCompress % 36) % 6;
      slen[3] = 0;
      sfcIdx = 3;
    } else if (sfCompress < 244) {
      /* max slen = floor[63/16] = 3 */
      sfCompress -= 180;
      slen[0] = (sfCompress & 0x3f) >> 4;
      slen[1] = (sfCompress & 0x0f) >> 2;
      slen[2] = (sfCompress & 0x03);
      slen[3] = 0;
      sfcIdx = 4;
    } else {
      /* max slen = floor[11/3] = 3 (max sfCompress >> 1 = 511/2 = 255) */
      sfCompress -= 244;
      slen[0] = (sfCompress / 3);
      slen[1] = (sfCompress % 3);
      slen[2] = slen[3] = 0;
      sfcIdx = 5;
    }
  }

  /* set index based on block type: (0,1,3) --> 0, (2 non-mixed) --> 1, (2
   * mixed) ---> 2 */
  btIdx = 0;
  if (sis->blockType == 2)
    btIdx = (sis->mixedBlock ? 2 : 1);
  for (i = 0; i < 4; i++)
    nr[i] = (int) NRTab[sfcIdx][btIdx][i];

  /* save intensity stereo scale factor info */
  if ((modeExt & 0x01) && (ch == 1)) {
    for (i = 0; i < 4; i++) {
      sfjs->slen[i] = slen[i];
      sfjs->nr[i] = nr[i];
    }
    sfjs->intensityScale = intensityScale;
  }
  sis->preFlag = preFlag;

  /* short blocks */
  if (sis->blockType == 2) {
    if (sis->mixedBlock) {
      /* do long block portion */
      iipTest = (1 << slen[0]) - 1;
      for (sfb = 0; sfb < 6; sfb++) {
        sfis->l[sfb] = (char) GetBits(bsi, slen[0]);
      }
      sfb = 3; /* start sfb for short */
      nrIdx = 1;
    } else {
      /* all short blocks, so start nr, sfb at 0 */
      sfb = 0;
      nrIdx = 0;
    }

    /* remaining short blocks, sfb just keeps incrementing */
    for (; nrIdx <= 3; nrIdx++) {
      iipTest = (1 << slen[nrIdx]) - 1;
      for (i = 0; i < nr[nrIdx]; i++, sfb++) {
        sfis->s[sfb][0] = (char) GetBits(bsi, slen[nrIdx]);
        sfis->s[sfb][1] = (char) GetBits(bsi, slen[nrIdx]);
        sfis->s[sfb][2] = (char) GetBits(bsi, slen[nrIdx]);
      }
    }
    /* last sf band not transmitted */
    sfis->s[12][0] = sfis->s[12][1] = sfis->s[12][2] = 0;
  } else {
    /* long blocks */
    sfb = 0;
    for (nrIdx = 0; nrIdx <= 3; nrIdx++) {
      iipTest = (1 << slen[nrIdx]) - 1;
      for (i = 0; i < nr[nrIdx]; i++, sfb++) {
        sfis->l[sfb] = (char) GetBits(bsi, slen[nrIdx]);
      }
    }
    /* last sf band not transmitted */
    sfis->l[21] = sfis->l[22] = 0;
  }
}

/**************************************************************************************
 * Function:    UnpackScaleFactors
 *
 * Description: parse the fields of the MP3 scale factor data section
 *
 * Inputs:      MP3DecInfo structure filled by UnpackFrameHeader() and
 *UnpackSideInfo() buffer pointing to the MP3 scale factor data pointer to bit
 *offset (0-7) indicating starting bit in buf[0] number of bits available in
 *data buffer index of current granule and channel
 *
 * Outputs:     updated platform-specific ScaleFactorInfo struct
 *              updated bitOffset
 *
 * Return:      length (in bytes) of scale factor data, -1 if null input
 *pointers
 **************************************************************************************/
int UnpackScaleFactors(MP3DecInfo *mp3DecInfo, unsigned char *buf, int *bitOffset, int bitsAvail, int gr, int ch) {
  int bitsUsed;
  unsigned char *startBuf;
  BitStreamInfo bitStreamInfo, *bsi;
  FrameHeader *fh;
  SideInfo *si;
  ScaleFactorInfo *sfi;

  /* validate pointers */
  if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || !mp3DecInfo->SideInfoPS || !mp3DecInfo->ScaleFactorInfoPS)
    return -1;
  fh = ((FrameHeader *) (mp3DecInfo->FrameHeaderPS));
  si = ((SideInfo *) (mp3DecInfo->SideInfoPS));
  sfi = ((ScaleFactorInfo *) (mp3DecInfo->ScaleFactorInfoPS));

  /* init GetBits reader */
  startBuf = buf;
  bsi = &bitStreamInfo;
  SetBitstreamPointer(bsi, (bitsAvail + *bitOffset + 7) / 8, buf);
  if (*bitOffset)
    GetBits(bsi, *bitOffset);

  if (fh->ver == MPEG1)
    UnpackSFMPEG1(bsi, &si->sis[gr][ch], &sfi->sfis[gr][ch], si->scfsi[ch], gr, &sfi->sfis[0][ch]);
  else
    UnpackSFMPEG2(bsi, &si->sis[gr][ch], &sfi->sfis[gr][ch], gr, ch, fh->modeExt, &sfi->sfjs);

  mp3DecInfo->part23Length[gr][ch] = si->sis[gr][ch].part23Length;

  bitsUsed = CalcBitsUsed(bsi, buf, *bitOffset);
  buf += (bitsUsed + *bitOffset) >> 3;
  *bitOffset = (bitsUsed + *bitOffset) & 0x07;

  return (buf - startBuf);
}

/**************************************************************************************
 * Function:    AntiAlias
 *
 * Description: smooth transition across DCT block boundaries (every 18
 *coefficients)
 *
 * Inputs:      vector of dequantized coefficients, length = (nBfly+1) * 18
 *              number of "butterflies" to perform (one butterfly means one
 *                inter-block smoothing operation)
 *
 * Outputs:     updated coefficient vector x
 *
 * Return:      none
 *
 * Notes:       weighted average of opposite bands (pairwise) from the 8 samples
 *                before and after each block boundary
 *              nBlocks = (nonZeroBound + 7) / 18, since nZB is the first ZERO
 *sample above which all other samples are also zero max gain per sample = 1.372
 *                MAX(i) (abs(csa[i][0]) + abs(csa[i][1]))
 *              bits gained = 0
 *              assume at least 1 guard bit in x[] to avoid overflow
 *                (should be guaranteed from dequant, and max gain from stproc *
 *max gain from AntiAlias < 2.0)
 **************************************************************************************/
// a little bit faster in RAM (< 1 ms per block)
static void AntiAlias(int *x, int nBfly) {
  int k, a0, b0, c0, c1;
  const uint32_t *c;

  /* csa = Q31 */
  for (k = nBfly; k > 0; k--) {
    c = csa[0];
    x += 18;

    a0 = x[-1];
    c0 = *c;
    c++;
    b0 = x[0];
    c1 = *c;
    c++;
    x[-1] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;
    x[0] = (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

    a0 = x[-2];
    c0 = *c;
    c++;
    b0 = x[1];
    c1 = *c;
    c++;
    x[-2] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;
    x[1] = (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

    a0 = x[-3];
    c0 = *c;
    c++;
    b0 = x[2];
    c1 = *c;
    c++;
    x[-3] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;
    x[2] = (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

    a0 = x[-4];
    c0 = *c;
    c++;
    b0 = x[3];
    c1 = *c;
    c++;
    x[-4] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;
    x[3] = (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

    a0 = x[-5];
    c0 = *c;
    c++;
    b0 = x[4];
    c1 = *c;
    c++;
    x[-5] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;
    x[4] = (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

    a0 = x[-6];
    c0 = *c;
    c++;
    b0 = x[5];
    c1 = *c;
    c++;
    x[-6] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;
    x[5] = (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

    a0 = x[-7];
    c0 = *c;
    c++;
    b0 = x[6];
    c1 = *c;
    c++;
    x[-7] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;
    x[6] = (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;

    a0 = x[-8];
    c0 = *c;
    c++;
    b0 = x[7];
    c1 = *c;
    c++;
    x[-8] = (MULSHIFT32(c0, a0) - MULSHIFT32(c1, b0)) << 1;
    x[7] = (MULSHIFT32(c0, b0) + MULSHIFT32(c1, a0)) << 1;
  }
}

/**************************************************************************************
 * Function:    WinPrevious
 *
 * Description: apply specified window to second half of previous IMDCT (overlap
 *part)
 *
 * Inputs:      vector of 9 coefficients (xPrev)
 *
 * Outputs:     18 windowed output coefficients (gain 1 integer bit)
 *              window type (0, 1, 2, 3)
 *
 * Return:      none
 *
 * Notes:       produces 9 output samples from 18 input samples via symmetry
 *              all blocks gain at least 1 guard bit via window (long blocks get
 *extra sign bit, short blocks can have one addition but max gain < 1.0)
 **************************************************************************************/
static void WinPrevious(int *xPrev, int *xPrevWin, int btPrev) {
  int i, x, *xp, *xpwLo, *xpwHi, wLo, wHi;
  const uint32_t *wpLo, *wpHi;

  xp = xPrev;
  /* mapping (see IMDCT12x3): xPrev[0-2] = sum[6-8], xPrev[3-8] = sum[12-17] */
  if (btPrev == 2) {
    /* this could be reordered for minimum loads/stores */
    wpLo = imdctWin[btPrev];
    xPrevWin[0] = MULSHIFT32(wpLo[6], xPrev[2]) + MULSHIFT32(wpLo[0], xPrev[6]);
    xPrevWin[1] = MULSHIFT32(wpLo[7], xPrev[1]) + MULSHIFT32(wpLo[1], xPrev[7]);
    xPrevWin[2] = MULSHIFT32(wpLo[8], xPrev[0]) + MULSHIFT32(wpLo[2], xPrev[8]);
    xPrevWin[3] = MULSHIFT32(wpLo[9], xPrev[0]) + MULSHIFT32(wpLo[3], xPrev[8]);
    xPrevWin[4] = MULSHIFT32(wpLo[10], xPrev[1]) + MULSHIFT32(wpLo[4], xPrev[7]);
    xPrevWin[5] = MULSHIFT32(wpLo[11], xPrev[2]) + MULSHIFT32(wpLo[5], xPrev[6]);
    xPrevWin[6] = MULSHIFT32(wpLo[6], xPrev[5]);
    xPrevWin[7] = MULSHIFT32(wpLo[7], xPrev[4]);
    xPrevWin[8] = MULSHIFT32(wpLo[8], xPrev[3]);
    xPrevWin[9] = MULSHIFT32(wpLo[9], xPrev[3]);
    xPrevWin[10] = MULSHIFT32(wpLo[10], xPrev[4]);
    xPrevWin[11] = MULSHIFT32(wpLo[11], xPrev[5]);
    xPrevWin[12] = xPrevWin[13] = xPrevWin[14] = xPrevWin[15] = xPrevWin[16] = xPrevWin[17] = 0;
  } else {
    /* use ARM-style pointers (*ptr++) so that ADS compiles well */
    wpLo = imdctWin[btPrev] + 18;
    wpHi = wpLo + 17;
    xpwLo = xPrevWin;
    xpwHi = xPrevWin + 17;
    for (i = 9; i > 0; i--) {
      x = *xp++;
      wLo = *wpLo++;
      wHi = *wpHi--;
      *xpwLo++ = MULSHIFT32(wLo, x);
      *xpwHi-- = MULSHIFT32(wHi, x);
    }
  }
}

/**************************************************************************************
 * Function:    FreqInvertRescale
 *
 * Description: do frequency inversion (odd samples of odd blocks) and rescale
 *                if necessary (extra guard bits added before IMDCT)
 *
 * Inputs:      output vector y (18 new samples, spaced NBANDS apart)
 *              previous sample vector xPrev (9 samples)
 *              index of current block
 *              number of extra shifts added before IMDCT (usually 0)
 *
 * Outputs:     inverted and rescaled (as necessary) outputs
 *              rescaled (as necessary) previous samples
 *
 * Return:      updated mOut (from new outputs y)
 **************************************************************************************/
static int FreqInvertRescale(int *y, int *xPrev, int blockIdx, int es) {
  int i, d, mOut;
  int y0, y1, y2, y3, y4, y5, y6, y7, y8;

  if (es == 0) {
    /* fast case - frequency invert only (no rescaling) - can fuse into
     * overlap-add for speed, if desired */
    if (blockIdx & 0x01) {
      y += NBANDS;
      y0 = *y;
      y += 2 * NBANDS;
      y1 = *y;
      y += 2 * NBANDS;
      y2 = *y;
      y += 2 * NBANDS;
      y3 = *y;
      y += 2 * NBANDS;
      y4 = *y;
      y += 2 * NBANDS;
      y5 = *y;
      y += 2 * NBANDS;
      y6 = *y;
      y += 2 * NBANDS;
      y7 = *y;
      y += 2 * NBANDS;
      y8 = *y;
      y += 2 * NBANDS;

      y -= 18 * NBANDS;
      *y = -y0;
      y += 2 * NBANDS;
      *y = -y1;
      y += 2 * NBANDS;
      *y = -y2;
      y += 2 * NBANDS;
      *y = -y3;
      y += 2 * NBANDS;
      *y = -y4;
      y += 2 * NBANDS;
      *y = -y5;
      y += 2 * NBANDS;
      *y = -y6;
      y += 2 * NBANDS;
      *y = -y7;
      y += 2 * NBANDS;
      *y = -y8;
      y += 2 * NBANDS;
    }
    return 0;
  } else {
    /* undo pre-IMDCT scaling, clipping if necessary */
    mOut = 0;
    if (blockIdx & 0x01) {
      /* frequency invert */
      for (i = 0; i < 18; i += 2) {
        d = *y;
        CLIP_2N(d, 31 - es);
        *y = d << es;
        mOut |= FASTABS(*y);
        y += NBANDS;
        d = -*y;
        CLIP_2N(d, 31 - es);
        *y = d << es;
        mOut |= FASTABS(*y);
        y += NBANDS;
        d = *xPrev;
        CLIP_2N(d, 31 - es);
        *xPrev++ = d << es;
      }
    } else {
      for (i = 0; i < 18; i += 2) {
        d = *y;
        CLIP_2N(d, 31 - es);
        *y = d << es;
        mOut |= FASTABS(*y);
        y += NBANDS;
        d = *y;
        CLIP_2N(d, 31 - es);
        *y = d << es;
        mOut |= FASTABS(*y);
        y += NBANDS;
        d = *xPrev;
        CLIP_2N(d, 31 - es);
        *xPrev++ = d << es;
      }
    }
    return mOut;
  }
}

/* format = Q31
 * #define M_PI 3.14159265358979323846
 * double u = 2.0 * M_PI / 9.0;
 * float c0 = sqrt(3.0) / 2.0;
 * float c1 = cos(u);
 * float c2 = cos(2*u);
 * float c3 = sin(u);
 * float c4 = sin(2*u);
 */

static const int c9_0 = 0x6ed9eba1;
static const int c9_1 = 0x620dbe8b;
static const int c9_2 = 0x163a1a7e;
static const int c9_3 = 0x5246dd49;
static const int c9_4 = 0x7e0e2e32;

/* format = Q31
 * cos(((0:8) + 0.5) * (pi/18))
 */
static const uint32_t c18[9] = {
    0x7f834ed0, 0x7ba3751d, 0x7401e4c1, 0x68d9f964, 0x5a82799a, 0x496af3e2, 0x36185aee, 0x2120fb83, 0x0b27eb5c,
};

/* require at least 3 guard bits in x[] to ensure no overflow */
static __inline void idct9(int *x) {
  int a1, a2, a3, a4, a5, a6, a7, a8, a9;
  int a10, a11, a12, a13, a14, a15, a16, a17, a18;
  int a19, a20, a21, a22, a23, a24, a25, a26, a27;
  int m1, m3, m5, m6, m7, m8, m9, m10, m11, m12;
  int x0, x1, x2, x3, x4, x5, x6, x7, x8;

  x0 = x[0];
  x1 = x[1];
  x2 = x[2];
  x3 = x[3];
  x4 = x[4];
  x5 = x[5];
  x6 = x[6];
  x7 = x[7];
  x8 = x[8];

  a1 = x0 - x6;
  a2 = x1 - x5;
  a3 = x1 + x5;
  a4 = x2 - x4;
  a5 = x2 + x4;
  a6 = x2 + x8;
  a7 = x1 + x7;

  a8 = a6 - a5;  /* ie x[8] - x[4] */
  a9 = a3 - a7;  /* ie x[5] - x[7] */
  a10 = a2 - x7; /* ie x[1] - x[5] - x[7] */
  a11 = a4 - x8; /* ie x[2] - x[4] - x[8] */

  /* do the << 1 as constant shifts where mX is actually used (free, no stall or
   * extra inst.) */
  m1 = MULSHIFT32(c9_0, x3);
  m3 = MULSHIFT32(c9_0, a10);
  m5 = MULSHIFT32(c9_1, a5);
  m6 = MULSHIFT32(c9_2, a6);
  m7 = MULSHIFT32(c9_1, a8);
  m8 = MULSHIFT32(c9_2, a5);
  m9 = MULSHIFT32(c9_3, a9);
  m10 = MULSHIFT32(c9_4, a7);
  m11 = MULSHIFT32(c9_3, a3);
  m12 = MULSHIFT32(c9_4, a9);

  a12 = x[0] + (x[6] >> 1);
  a13 = a12 + (m1 << 1);
  a14 = a12 - (m1 << 1);
  a15 = a1 + (a11 >> 1);
  a16 = (m5 << 1) + (m6 << 1);
  a17 = (m7 << 1) - (m8 << 1);
  a18 = a16 + a17;
  a19 = (m9 << 1) + (m10 << 1);
  a20 = (m11 << 1) - (m12 << 1);

  a21 = a20 - a19;
  a22 = a13 + a16;
  a23 = a14 + a16;
  a24 = a14 + a17;
  a25 = a13 + a17;
  a26 = a14 - a18;
  a27 = a13 - a18;

  x0 = a22 + a19;
  x[0] = x0;
  x1 = a15 + (m3 << 1);
  x[1] = x1;
  x2 = a24 + a20;
  x[2] = x2;
  x3 = a26 - a21;
  x[3] = x3;
  x4 = a1 - a11;
  x[4] = x4;
  x5 = a27 + a21;
  x[5] = x5;
  x6 = a25 - a20;
  x[6] = x6;
  x7 = a15 - (m3 << 1);
  x[7] = x7;
  x8 = a23 - a19;
  x[8] = x8;
}

/* let c(j) = cos(M_PI/36 * ((j)+0.5)), s(j) = sin(M_PI/36 * ((j)+0.5))
 * then fastWin[2*j+0] = c(j)*(s(j) + c(j)), j = [0, 8]
 *      fastWin[2*j+1] = c(j)*(s(j) - c(j))
 * format = Q30
 */
const uint32_t fastWin36[18] = {
    0x42aace8b, 0xc2e92724, 0x47311c28, 0xc95f619a, 0x4a868feb, 0xd0859d8c, 0x4c913b51, 0xd8243ea0, 0x4d413ccc,
    0xe0000000, 0x4c913b51, 0xe7dbc161, 0x4a868feb, 0xef7a6275, 0x47311c28, 0xf6a09e67, 0x42aace8b, 0xfd16d8dd,
};

/**************************************************************************************
 * Function:    IMDCT36
 *
 * Description: 36-point modified DCT, with windowing and overlap-add (50%
 *overlap)
 *
 * Inputs:      vector of 18 coefficients (N/2 inputs produces N outputs, by
 *symmetry) overlap part of last IMDCT (9 samples - see output comments) window
 *type (0,1,2,3) of current and previous block current block index (for deciding
 *whether to do frequency inversion) number of guard bits in input vector
 *
 * Outputs:     18 output samples, after windowing and overlap-add with last
 *frame second half of (unwindowed) 36-point IMDCT - save for next time only
 *save 9 xPrev samples, using symmetry (see WinPrevious())
 *
 * Notes:       this is Ken's hyper-fast algorithm, including symmetric sin
 *window optimization, if applicable total number of multiplies, general case:
 *                2*10 (idct9) + 9 (last stage imdct) + 36 (for windowing) = 65
 *              total number of multiplies, btCurr == 0 && btPrev == 0:
 *                2*10 (idct9) + 9 (last stage imdct) + 18 (for windowing) = 47
 *
 *              blockType == 0 is by far the most common case, so it should be
 *                possible to use the fast path most of the time
 *              this is the fastest known algorithm for performing
 *                long IMDCT + windowing + overlap-add in MP3
 *
 * Return:      mOut (OR of abs(y) for all y calculated here)
 *
 * TODO:        optimize for ARM (reorder window coefs, ARM-style pointers in C,
 *                inline asm may or may not be helpful)
 **************************************************************************************/
// barely faster in RAM
static int IMDCT36(int *xCurr, int *xPrev, int *y, int btCurr, int btPrev, int blockIdx, int gb) {
  int i, es, xBuf[18], xPrevWin[18];
  int acc1, acc2, s, d, t, mOut;
  int xo, xe, c, *xp, yLo, yHi;
  const uint32_t *cp, *wp;

  acc1 = acc2 = 0;
  xCurr += 17;

  /* 7 gb is always adequate for antialias + accumulator loop + idct9 */
  if (gb < 7) {
    /* rarely triggered - 5% to 10% of the time on normal clips (with Q25 input)
     */
    es = 7 - gb;
    for (i = 8; i >= 0; i--) {
      acc1 = ((*xCurr--) >> es) - acc1;
      acc2 = acc1 - acc2;
      acc1 = ((*xCurr--) >> es) - acc1;
      xBuf[i + 9] = acc2; /* odd */
      xBuf[i + 0] = acc1; /* even */
      xPrev[i] >>= es;
    }
  } else {
    es = 0;
    /* max gain = 18, assume adequate guard bits */
    for (i = 8; i >= 0; i--) {
      acc1 = (*xCurr--) - acc1;
      acc2 = acc1 - acc2;
      acc1 = (*xCurr--) - acc1;
      xBuf[i + 9] = acc2; /* odd */
      xBuf[i + 0] = acc1; /* even */
    }
  }
  /* xEven[0] and xOdd[0] scaled by 0.5 */
  xBuf[9] >>= 1;
  xBuf[0] >>= 1;

  /* do 9-point IDCT on even and odd */
  idct9(xBuf + 0); /* even */
  idct9(xBuf + 9); /* odd */

  xp = xBuf + 8;
  cp = c18 + 8;
  mOut = 0;
  if (btPrev == 0 && btCurr == 0) {
    /* fast path - use symmetry of sin window to reduce windowing multiplies to
     * 18 (N/2) */
    wp = fastWin36;
    for (i = 0; i < 9; i++) {
      /* do ARM-style pointer arithmetic (i still needed for y[] indexing -
       * compiler spills if 2 y pointers) */
      c = *cp--;
      xo = *(xp + 9);
      xe = *xp--;
      /* gain 2 int bits here */
      xo = MULSHIFT32(c, xo); /* 2*c18*xOdd (mul by 2 implicit in scaling)  */
      xe >>= 2;

      s = -(*xPrev);        /* sum from last block (always at least 2 guard bits) */
      d = -(xe - xo);       /* gain 2 int bits, don't shift xo (effective << 1 to eat
                               sign bit, << 1 for mul by 2) */
      (*xPrev++) = xe + xo; /* symmetry - xPrev[i] = xPrev[17-i] for long blocks */
      t = s - d;

      yLo = (d + (MULSHIFT32(t, *wp++) << 2));
      yHi = (s + (MULSHIFT32(t, *wp++) << 2));
      y[(i) *NBANDS] = yLo;
      y[(17 - i) * NBANDS] = yHi;
      mOut |= FASTABS(yLo);
      mOut |= FASTABS(yHi);
    }
  } else {
    /* slower method - either prev or curr is using window type != 0 so do full
     * 36-point window output xPrevWin has at least 3 guard bits (xPrev has 2,
     * gain 1 in WinPrevious)
     */
    WinPrevious(xPrev, xPrevWin, btPrev);

    wp = imdctWin[btCurr];
    for (i = 0; i < 9; i++) {
      c = *cp--;
      xo = *(xp + 9);
      xe = *xp--;
      /* gain 2 int bits here */
      xo = MULSHIFT32(c, xo); /* 2*c18*xOdd (mul by 2 implicit in scaling)  */
      xe >>= 2;

      d = xe - xo;
      (*xPrev++) = xe + xo; /* symmetry - xPrev[i] = xPrev[17-i] for long blocks */

      yLo = (xPrevWin[i] + MULSHIFT32(d, wp[i])) << 2;
      yHi = (xPrevWin[17 - i] + MULSHIFT32(d, wp[17 - i])) << 2;
      y[(i) *NBANDS] = yLo;
      y[(17 - i) * NBANDS] = yHi;
      mOut |= FASTABS(yLo);
      mOut |= FASTABS(yHi);
    }
  }

  xPrev -= 9;
  mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);

  return mOut;
}

static int c3_0 = 0x6ed9eba1;                            /* format = Q31, cos(pi/6) */
static int c6[3] = {0x7ba3751d, 0x5a82799a, 0x2120fb83}; /* format = Q31, cos(((0:2) + 0.5) * (pi/6)) */

/* 12-point inverse DCT, used in IMDCT12x3()
 * 4 input guard bits will ensure no overflow
 */
static __inline void imdct12(int *x, int *out) {
  int a0, a1, a2;
  int x0, x1, x2, x3, x4, x5;

  x0 = *x;
  x += 3;
  x1 = *x;
  x += 3;
  x2 = *x;
  x += 3;
  x3 = *x;
  x += 3;
  x4 = *x;
  x += 3;
  x5 = *x;
  x += 3;

  x4 -= x5;
  x3 -= x4;
  x2 -= x3;
  x3 -= x5;
  x1 -= x2;
  x0 -= x1;
  x1 -= x3;

  x0 >>= 1;
  x1 >>= 1;

  a0 = MULSHIFT32(c3_0, x2) << 1;
  a1 = x0 + (x4 >> 1);
  a2 = x0 - x4;
  x0 = a1 + a0;
  x2 = a2;
  x4 = a1 - a0;

  a0 = MULSHIFT32(c3_0, x3) << 1;
  a1 = x1 + (x5 >> 1);
  a2 = x1 - x5;

  /* cos window odd samples, mul by 2, eat sign bit */
  x1 = MULSHIFT32(c6[0], a1 + a0) << 2;
  x3 = MULSHIFT32(c6[1], a2) << 2;
  x5 = MULSHIFT32(c6[2], a1 - a0) << 2;

  *out = x0 + x1;
  out++;
  *out = x2 + x3;
  out++;
  *out = x4 + x5;
  out++;
  *out = x4 - x5;
  out++;
  *out = x2 - x3;
  out++;
  *out = x0 - x1;
}

/**************************************************************************************
 * Function:    IMDCT12x3
 *
 * Description: three 12-point modified DCT's for short blocks, with windowing,
 *                short block concatenation, and overlap-add
 *
 * Inputs:      3 interleaved vectors of 6 samples each
 *                (block0[0], block1[0], block2[0], block0[1], block1[1]....)
 *              overlap part of last IMDCT (9 samples - see output comments)
 *              window type (0,1,2,3) of previous block
 *              current block index (for deciding whether to do frequency
 *inversion) number of guard bits in input vector
 *
 * Outputs:     updated sample vector x, net gain of 1 integer bit
 *              second half of (unwindowed) IMDCT's - save for next time
 *                only save 9 xPrev samples, using symmetry (see WinPrevious())
 *
 * Return:      mOut (OR of abs(y) for all y calculated here)
 *
 * TODO:        optimize for ARM
 **************************************************************************************/
// barely faster in RAM
static int IMDCT12x3(int *xCurr, int *xPrev, int *y, int btPrev, int blockIdx, int gb) {
  int i, es, mOut, yLo, xBuf[18], xPrevWin[18]; /* need temp buffer for reordering short blocks */
  const uint32_t *wp;

  es = 0;
  /* 7 gb is always adequate for accumulator loop + idct12 + window + overlap */
  if (gb < 7) {
    es = 7 - gb;
    for (i = 0; i < 18; i += 2) {
      xCurr[i + 0] >>= es;
      xCurr[i + 1] >>= es;
      *xPrev++ >>= es;
    }
    xPrev -= 9;
  }

  /* requires 4 input guard bits for each imdct12 */
  imdct12(xCurr + 0, xBuf + 0);
  imdct12(xCurr + 1, xBuf + 6);
  imdct12(xCurr + 2, xBuf + 12);

  /* window previous from last time */
  WinPrevious(xPrev, xPrevWin, btPrev);

  /* could unroll this for speed, minimum loads (short blocks usually rare, so
   * doesn't make much overall difference) xPrevWin[i] << 2 still has 1 gb
   * always, max gain of windowed xBuf stuff also < 1.0 and gain the sign bit so
   * y calculations won't overflow
   */
  wp = imdctWin[2];
  mOut = 0;
  for (i = 0; i < 3; i++) {
    yLo = (xPrevWin[0 + i] << 2);
    mOut |= FASTABS(yLo);
    y[(0 + i) * NBANDS] = yLo;
    yLo = (xPrevWin[3 + i] << 2);
    mOut |= FASTABS(yLo);
    y[(3 + i) * NBANDS] = yLo;
    yLo = (xPrevWin[6 + i] << 2) + (MULSHIFT32(wp[0 + i], xBuf[3 + i]));
    mOut |= FASTABS(yLo);
    y[(6 + i) * NBANDS] = yLo;
    yLo = (xPrevWin[9 + i] << 2) + (MULSHIFT32(wp[3 + i], xBuf[5 - i]));
    mOut |= FASTABS(yLo);
    y[(9 + i) * NBANDS] = yLo;
    yLo = (xPrevWin[12 + i] << 2) + (MULSHIFT32(wp[6 + i], xBuf[2 - i]) + MULSHIFT32(wp[0 + i], xBuf[(6 + 3) + i]));
    mOut |= FASTABS(yLo);
    y[(12 + i) * NBANDS] = yLo;
    yLo = (xPrevWin[15 + i] << 2) + (MULSHIFT32(wp[9 + i], xBuf[0 + i]) + MULSHIFT32(wp[3 + i], xBuf[(6 + 5) - i]));
    mOut |= FASTABS(yLo);
    y[(15 + i) * NBANDS] = yLo;
  }

  /* save previous (unwindowed) for overlap - only need samples 6-8, 12-17 */
  for (i = 6; i < 9; i++)
    *xPrev++ = xBuf[i] >> 2;
  for (i = 12; i < 18; i++)
    *xPrev++ = xBuf[i] >> 2;

  xPrev -= 9;
  mOut |= FreqInvertRescale(y, xPrev, blockIdx, es);

  return mOut;
}

/**************************************************************************************
 * Function:    HybridTransform
 *
 * Description: IMDCT's, windowing, and overlap-add on long/short/mixed blocks
 *
 * Inputs:      vector of input coefficients, length = nBlocksTotal * 18)
 *              vector of overlap samples from last time, length = nBlocksPrev *
 *9) buffer for output samples, length = MAXNSAMP SideInfoSub struct for this
 *granule/channel BlockCount struct with necessary info number of non-zero input
 *and overlap blocks number of long blocks in input vector (rest assumed to be
 *short blocks) number of blocks which use long window (type) 0 in case of mixed
 *block (bc->currWinSwitch, 0 for non-mixed blocks)
 *
 * Outputs:     transformed, windowed, and overlapped sample buffer
 *              does frequency inversion on odd blocks
 *              updated buffer of samples for overlap
 *
 * Return:      number of non-zero IMDCT blocks calculated in this call
 *                (including overlap-add)
 *
 * TODO:        examine mixedBlock/winSwitch logic carefully (test he_mode.bit)
 **************************************************************************************/
static int HybridTransform(int *xCurr, int *xPrev, int y[BLOCK_SIZE][NBANDS], SideInfoSub *sis, BlockCount *bc) {
  int xPrevWin[18], currWinIdx, prevWinIdx;
  int i, j, nBlocksOut, nonZero, mOut;
  int fiBit, xp;

  ASSERT(bc->nBlocksLong <= NBANDS);
  ASSERT(bc->nBlocksTotal <= NBANDS);
  ASSERT(bc->nBlocksPrev <= NBANDS);

  mOut = 0;

  /* do long blocks, if any */
  for (i = 0; i < bc->nBlocksLong; i++) {
    /* currWinIdx picks the right window for long blocks (if mixed, long blocks
     * use window type 0) */
    currWinIdx = sis->blockType;
    if (sis->mixedBlock && i < bc->currWinSwitch)
      currWinIdx = 0;

    prevWinIdx = bc->prevType;
    if (i < bc->prevWinSwitch)
      prevWinIdx = 0;

    /* do 36-point IMDCT, including windowing and overlap-add */
    mOut |= IMDCT36(xCurr, xPrev, &(y[0][i]), currWinIdx, prevWinIdx, i, bc->gbIn);
    xCurr += 18;
    xPrev += 9;
  }

  /* do short blocks (if any) */
  for (; i < bc->nBlocksTotal; i++) {
    ASSERT(sis->blockType == 2);

    prevWinIdx = bc->prevType;
    if (i < bc->prevWinSwitch)
      prevWinIdx = 0;

    mOut |= IMDCT12x3(xCurr, xPrev, &(y[0][i]), prevWinIdx, i, bc->gbIn);
    xCurr += 18;
    xPrev += 9;
  }
  nBlocksOut = i;

  /* window and overlap prev if prev longer that current */
  for (; i < bc->nBlocksPrev; i++) {
    prevWinIdx = bc->prevType;
    if (i < bc->prevWinSwitch)
      prevWinIdx = 0;
    WinPrevious(xPrev, xPrevWin, prevWinIdx);

    nonZero = 0;
    fiBit = i << 31;
    for (j = 0; j < 9; j++) {
      xp = xPrevWin[2 * j + 0] << 2; /* << 2 temp for scaling */
      nonZero |= xp;
      y[2 * j + 0][i] = xp;
      mOut |= FASTABS(xp);

      /* frequency inversion on odd blocks/odd samples (flip sign if i odd, j
       * odd) */
      xp = xPrevWin[2 * j + 1] << 2;
      xp = (xp ^ (fiBit >> 31)) + (i & 0x01);
      nonZero |= xp;
      y[2 * j + 1][i] = xp;
      mOut |= FASTABS(xp);

      xPrev[j] = 0;
    }
    xPrev += 9;
    if (nonZero)
      nBlocksOut = i;
  }

  /* clear rest of blocks */
  for (; i < 32; i++) {
    for (j = 0; j < 18; j++)
      y[j][i] = 0;
  }

  bc->gbOut = CLZ(mOut) - 1;

  return nBlocksOut;
}

/**************************************************************************************
 * Function:    IMDCT
 *
 * Description: do alias reduction, inverse MDCT, overlap-add, and frequency
 *inversion
 *
 * Inputs:      MP3DecInfo structure filled by UnpackFrameHeader(),
 *UnpackSideInfo(), UnpackScaleFactors(), and DecodeHuffman() (for this granule,
 *channel) includes PCM samples in overBuf (from last call to IMDCT) for OLA
 *              index of current granule and channel
 *
 * Outputs:     PCM samples in outBuf, for input to subband transform
 *              PCM samples in overBuf, for OLA next time
 *              updated hi->nonZeroBound index for this channel
 *
 * Return:      0 on success,  -1 if null input pointers
 **************************************************************************************/
// a bit faster in RAM
int IMDCT(MP3DecInfo *mp3DecInfo, int gr, int ch) {
  int nBfly, blockCutoff;
  FrameHeader *fh;
  SideInfo *si;
  HuffmanInfo *hi;
  IMDCTInfo *mi;
  BlockCount bc;

  /* validate pointers */
  if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || !mp3DecInfo->SideInfoPS || !mp3DecInfo->HuffmanInfoPS ||
      !mp3DecInfo->IMDCTInfoPS)
    return -1;

  /* si is an array of up to 4 structs, stored as gr0ch0, gr0ch1, gr1ch0, gr1ch1
   */
  fh = (FrameHeader *) (mp3DecInfo->FrameHeaderPS);
  si = (SideInfo *) (mp3DecInfo->SideInfoPS);
  hi = (HuffmanInfo *) (mp3DecInfo->HuffmanInfoPS);
  mi = (IMDCTInfo *) (mp3DecInfo->IMDCTInfoPS);

  /* anti-aliasing done on whole long blocks only
   * for mixed blocks, nBfly always 1, except 3 for 8 kHz MPEG 2.5 (see
   * sfBandTab) nLongBlocks = number of blocks with (possibly) non-zero power
   *   nBfly = number of butterflies to do (nLongBlocks - 1, unless no long
   * blocks)
   */
  blockCutoff = fh->sfBand->l[(fh->ver == MPEG1 ? 8 : 6)] / 18; /* same as 3* num short sfb's in spec */
  if (si->sis[gr][ch].blockType != 2) {
    /* all long transforms */
    bc.nBlocksLong = MIN((hi->nonZeroBound[ch] + 7) / 18 + 1, 32);
    nBfly = bc.nBlocksLong - 1;
  } else if (si->sis[gr][ch].blockType == 2 && si->sis[gr][ch].mixedBlock) {
    /* mixed block - long transforms until cutoff, then short transforms */
    bc.nBlocksLong = blockCutoff;
    nBfly = bc.nBlocksLong - 1;
  } else {
    /* all short transforms */
    bc.nBlocksLong = 0;
    nBfly = 0;
  }

  AntiAlias(hi->huffDecBuf[ch], nBfly);
  hi->nonZeroBound[ch] = MAX(hi->nonZeroBound[ch], (nBfly * 18) + 8);

  ASSERT(hi->nonZeroBound[ch] <= MAX_NSAMP);

  /* for readability, use a struct instead of passing a million parameters to
   * HybridTransform() */
  bc.nBlocksTotal = (hi->nonZeroBound[ch] + 17) / 18;
  bc.nBlocksPrev = mi->numPrevIMDCT[ch];
  bc.prevType = mi->prevType[ch];
  bc.prevWinSwitch = mi->prevWinSwitch[ch];
  bc.currWinSwitch = (si->sis[gr][ch].mixedBlock ? blockCutoff : 0); /* where WINDOW switches (not nec. transform) */
  bc.gbIn = hi->gb[ch];

  mi->numPrevIMDCT[ch] = HybridTransform(hi->huffDecBuf[ch], mi->overBuf[ch], mi->outBuf[ch], &si->sis[gr][ch], &bc);
  mi->prevType[ch] = si->sis[gr][ch].blockType;
  mi->prevWinSwitch[ch] = bc.currWinSwitch; /* 0 means not a mixed block (either
                                               all short or all long) */
  mi->gb[ch] = bc.gbOut;

  ASSERT(mi->numPrevIMDCT[ch] <= NBANDS);

  /* output has gained 2 int bits */
  return 0;
}

/* NOTE - regenerated tables to use shorts instead of ints
 *        (all needed data can fit in 16 bits - see below)
 *
 * format 0xABCD
 *  A = length of codeword
 *  B = y value
 *  C = x value
 *  D = number of sign bits (0, 1, or 2)
 *
 * to read a CW, the code reads maxbits from the stream (dep. on
 *   table index), but doesn't remove them from the bitstream reader
 * then it gets the correct CW by direct lookup into the table
 *   of length (2^maxbits) (more complicated for non-oneShot...)
 * for CW's with hlen < maxbits, there are multiple entries in the
 *   table (extra bits are don't cares)
 * the bitstream reader then "purges" (or removes) only the correct
 *   number of bits for the chosen CW
 *
 * entries starting with F are special: D (signbits) is maxbits,
 *   so the decoder always checks huffTableXX[0] first, gets the
 *   signbits, and reads that many bits from the bitstream
 *   (sometimes it takes > 1 read to get the value, so maxbits is
 *    can get updated by jumping to another value starting with 0xF)
 * entries starting with 0 are also special: A = hlen = 0, rest of
 *   value is an offset to jump higher in the table (for tables of
 *   type loopNoLinbits or loopLinbits)
 */

/* store Huffman codes as one big table plus table of offsets, since some
 * platforms don't properly support table-of-tables (table of pointers to other
 * const tables)
 */
const unsigned short huffTable[] = {
    /* huffTable01[9] */
    0xf003,
    0x3112,
    0x3101,
    0x2011,
    0x2011,
    0x1000,
    0x1000,
    0x1000,
    0x1000,

    /* huffTable02[65] */
    0xf006,
    0x6222,
    0x6201,
    0x5212,
    0x5212,
    0x5122,
    0x5122,
    0x5021,
    0x5021,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,

    /* huffTable03[65] */
    0xf006,
    0x6222,
    0x6201,
    0x5212,
    0x5212,
    0x5122,
    0x5122,
    0x5021,
    0x5021,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2101,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,

    /* huffTable05[257] */
    0xf008,
    0x8332,
    0x8322,
    0x7232,
    0x7232,
    0x6132,
    0x6132,
    0x6132,
    0x6132,
    0x7312,
    0x7312,
    0x7301,
    0x7301,
    0x7031,
    0x7031,
    0x7222,
    0x7222,
    0x6212,
    0x6212,
    0x6212,
    0x6212,
    0x6122,
    0x6122,
    0x6122,
    0x6122,
    0x6201,
    0x6201,
    0x6201,
    0x6201,
    0x6021,
    0x6021,
    0x6021,
    0x6021,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,

    /* huffTable06[129] */
    0xf007,
    0x7332,
    0x7301,
    0x6322,
    0x6322,
    0x6232,
    0x6232,
    0x6031,
    0x6031,
    0x5312,
    0x5312,
    0x5312,
    0x5312,
    0x5132,
    0x5132,
    0x5132,
    0x5132,
    0x5222,
    0x5222,
    0x5222,
    0x5222,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4021,
    0x4021,
    0x4021,
    0x4021,
    0x4021,
    0x4021,
    0x4021,
    0x4021,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,

    /* huffTable07[110] */
    0xf006,
    0x0041,
    0x0052,
    0x005b,
    0x0060,
    0x0063,
    0x0068,
    0x006b,
    0x6212,
    0x5122,
    0x5122,
    0x6201,
    0x6021,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0xf004,
    0x4552,
    0x4542,
    0x4452,
    0x4352,
    0x3532,
    0x3532,
    0x3442,
    0x3442,
    0x3522,
    0x3522,
    0x3252,
    0x3252,
    0x2512,
    0x2512,
    0x2512,
    0x2512,
    0xf003,
    0x2152,
    0x2152,
    0x3501,
    0x3432,
    0x2051,
    0x2051,
    0x3342,
    0x3332,
    0xf002,
    0x2422,
    0x2242,
    0x1412,
    0x1412,
    0xf001,
    0x1142,
    0x1041,
    0xf002,
    0x2401,
    0x2322,
    0x2232,
    0x2301,
    0xf001,
    0x1312,
    0x1132,
    0xf001,
    0x1031,
    0x1222,

    /* huffTable08[280] */
    0xf008,
    0x0101,
    0x010a,
    0x010f,
    0x8512,
    0x8152,
    0x0112,
    0x0115,
    0x8422,
    0x8242,
    0x8412,
    0x7142,
    0x7142,
    0x8401,
    0x8041,
    0x8322,
    0x8232,
    0x8312,
    0x8132,
    0x8301,
    0x8031,
    0x6222,
    0x6222,
    0x6222,
    0x6222,
    0x6201,
    0x6201,
    0x6201,
    0x6201,
    0x6021,
    0x6021,
    0x6021,
    0x6021,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x2112,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0xf003,
    0x3552,
    0x3452,
    0x2542,
    0x2542,
    0x1352,
    0x1352,
    0x1352,
    0x1352,
    0xf002,
    0x2532,
    0x2442,
    0x1522,
    0x1522,
    0xf001,
    0x1252,
    0x1501,
    0xf001,
    0x1432,
    0x1342,
    0xf001,
    0x1051,
    0x1332,

    /* huffTable09[93] */
    0xf006,
    0x0041,
    0x004a,
    0x004f,
    0x0052,
    0x0057,
    0x005a,
    0x6412,
    0x6142,
    0x6322,
    0x6232,
    0x5312,
    0x5312,
    0x5132,
    0x5132,
    0x6301,
    0x6031,
    0x5222,
    0x5222,
    0x5201,
    0x5201,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4021,
    0x4021,
    0x4021,
    0x4021,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0xf003,
    0x3552,
    0x3542,
    0x2532,
    0x2532,
    0x2352,
    0x2352,
    0x3452,
    0x3501,
    0xf002,
    0x2442,
    0x2522,
    0x2252,
    0x2512,
    0xf001,
    0x1152,
    0x1432,
    0xf002,
    0x1342,
    0x1342,
    0x2051,
    0x2401,
    0xf001,
    0x1422,
    0x1242,
    0xf001,
    0x1332,
    0x1041,

    /* huffTable10[320] */
    0xf008,
    0x0101,
    0x010a,
    0x010f,
    0x0118,
    0x011b,
    0x0120,
    0x0125,
    0x8712,
    0x8172,
    0x012a,
    0x012d,
    0x0132,
    0x8612,
    0x8162,
    0x8061,
    0x0137,
    0x013a,
    0x013d,
    0x8412,
    0x8142,
    0x8041,
    0x8322,
    0x8232,
    0x8301,
    0x7312,
    0x7312,
    0x7132,
    0x7132,
    0x7031,
    0x7031,
    0x7222,
    0x7222,
    0x6212,
    0x6212,
    0x6212,
    0x6212,
    0x6122,
    0x6122,
    0x6122,
    0x6122,
    0x6201,
    0x6201,
    0x6201,
    0x6201,
    0x6021,
    0x6021,
    0x6021,
    0x6021,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0xf003,
    0x3772,
    0x3762,
    0x3672,
    0x3752,
    0x3572,
    0x3662,
    0x2742,
    0x2742,
    0xf002,
    0x2472,
    0x2652,
    0x2562,
    0x2732,
    0xf003,
    0x2372,
    0x2372,
    0x2642,
    0x2642,
    0x3552,
    0x3452,
    0x2362,
    0x2362,
    0xf001,
    0x1722,
    0x1272,
    0xf002,
    0x2462,
    0x2701,
    0x1071,
    0x1071,
    0xf002,
    0x1262,
    0x1262,
    0x2542,
    0x2532,
    0xf002,
    0x1601,
    0x1601,
    0x2352,
    0x2442,
    0xf001,
    0x1632,
    0x1622,
    0xf002,
    0x2522,
    0x2252,
    0x1512,
    0x1512,
    0xf002,
    0x1152,
    0x1152,
    0x2432,
    0x2342,
    0xf001,
    0x1501,
    0x1051,
    0xf001,
    0x1422,
    0x1242,
    0xf001,
    0x1332,
    0x1401,

    /* huffTable11[296] */
    0xf008,
    0x0101,
    0x0106,
    0x010f,
    0x0114,
    0x0117,
    0x8722,
    0x8272,
    0x011c,
    0x7172,
    0x7172,
    0x8712,
    0x8071,
    0x8632,
    0x8362,
    0x8061,
    0x011f,
    0x0122,
    0x8512,
    0x7262,
    0x7262,
    0x8622,
    0x8601,
    0x7612,
    0x7612,
    0x7162,
    0x7162,
    0x8152,
    0x8432,
    0x8051,
    0x0125,
    0x8422,
    0x8242,
    0x8412,
    0x8142,
    0x8401,
    0x8041,
    0x7322,
    0x7322,
    0x7232,
    0x7232,
    0x6312,
    0x6312,
    0x6312,
    0x6312,
    0x6132,
    0x6132,
    0x6132,
    0x6132,
    0x7301,
    0x7301,
    0x7031,
    0x7031,
    0x6222,
    0x6222,
    0x6222,
    0x6222,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0x2000,
    0xf002,
    0x2772,
    0x2762,
    0x2672,
    0x2572,
    0xf003,
    0x2662,
    0x2662,
    0x2742,
    0x2742,
    0x2472,
    0x2472,
    0x3752,
    0x3552,
    0xf002,
    0x2652,
    0x2562,
    0x1732,
    0x1732,
    0xf001,
    0x1372,
    0x1642,
    0xf002,
    0x2542,
    0x2452,
    0x2532,
    0x2352,
    0xf001,
    0x1462,
    0x1701,
    0xf001,
    0x1442,
    0x1522,
    0xf001,
    0x1252,
    0x1501,
    0xf001,
    0x1342,
    0x1332,

    /* huffTable12[185] */
    0xf007,
    0x0081,
    0x008a,
    0x008f,
    0x0092,
    0x0097,
    0x009a,
    0x009d,
    0x00a2,
    0x00a5,
    0x00a8,
    0x7622,
    0x7262,
    0x7162,
    0x00ad,
    0x00b0,
    0x00b3,
    0x7512,
    0x7152,
    0x7432,
    0x7342,
    0x00b6,
    0x7422,
    0x7242,
    0x7412,
    0x6332,
    0x6332,
    0x6142,
    0x6142,
    0x6322,
    0x6322,
    0x6232,
    0x6232,
    0x7041,
    0x7301,
    0x6031,
    0x6031,
    0x5312,
    0x5312,
    0x5312,
    0x5312,
    0x5132,
    0x5132,
    0x5132,
    0x5132,
    0x5222,
    0x5222,
    0x5222,
    0x5222,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4212,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x4122,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0xf003,
    0x3772,
    0x3762,
    0x2672,
    0x2672,
    0x2752,
    0x2752,
    0x2572,
    0x2572,
    0xf002,
    0x2662,
    0x2742,
    0x2472,
    0x2562,
    0xf001,
    0x1652,
    0x1732,
    0xf002,
    0x2372,
    0x2552,
    0x1722,
    0x1722,
    0xf001,
    0x1272,
    0x1642,
    0xf001,
    0x1462,
    0x1712,
    0xf002,
    0x1172,
    0x1172,
    0x2701,
    0x2071,
    0xf001,
    0x1632,
    0x1362,
    0xf001,
    0x1542,
    0x1452,
    0xf002,
    0x1442,
    0x1442,
    0x2601,
    0x2501,
    0xf001,
    0x1612,
    0x1061,
    0xf001,
    0x1532,
    0x1352,
    0xf001,
    0x1522,
    0x1252,
    0xf001,
    0x1051,
    0x1401,

    /* huffTable13[497] */
    0xf006,
    0x0041,
    0x0082,
    0x00c3,
    0x00e4,
    0x0105,
    0x0116,
    0x011f,
    0x0130,
    0x0139,
    0x013e,
    0x0143,
    0x0146,
    0x6212,
    0x6122,
    0x6201,
    0x6021,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0xf006,
    0x0108,
    0x0111,
    0x011a,
    0x0123,
    0x012c,
    0x0131,
    0x0136,
    0x013f,
    0x0144,
    0x0147,
    0x014c,
    0x0151,
    0x0156,
    0x015b,
    0x6f12,
    0x61f2,
    0x60f1,
    0x0160,
    0x0163,
    0x0166,
    0x62e2,
    0x0169,
    0x6e12,
    0x61e2,
    0x016c,
    0x016f,
    0x0172,
    0x0175,
    0x0178,
    0x017b,
    0x66c2,
    0x6d32,
    0x017e,
    0x6d22,
    0x62d2,
    0x6d12,
    0x67b2,
    0x0181,
    0x0184,
    0x63c2,
    0x0187,
    0x6b42,
    0x51d2,
    0x51d2,
    0x6d01,
    0x60d1,
    0x6a82,
    0x68a2,
    0x6c42,
    0x64c2,
    0x6b62,
    0x66b2,
    0x5c32,
    0x5c32,
    0x5c22,
    0x5c22,
    0x52c2,
    0x52c2,
    0x5b52,
    0x5b52,
    0x65b2,
    0x6982,
    0x5c12,
    0x5c12,
    0xf006,
    0x51c2,
    0x51c2,
    0x6892,
    0x6c01,
    0x50c1,
    0x50c1,
    0x64b2,
    0x6a62,
    0x66a2,
    0x6972,
    0x5b32,
    0x5b32,
    0x53b2,
    0x53b2,
    0x6882,
    0x6a52,
    0x5b22,
    0x5b22,
    0x65a2,
    0x6962,
    0x54a2,
    0x54a2,
    0x6872,
    0x6782,
    0x5492,
    0x5492,
    0x6772,
    0x6672,
    0x42b2,
    0x42b2,
    0x42b2,
    0x42b2,
    0x4b12,
    0x4b12,
    0x4b12,
    0x4b12,
    0x41b2,
    0x41b2,
    0x41b2,
    0x41b2,
    0x5b01,
    0x5b01,
    0x50b1,
    0x50b1,
    0x5692,
    0x5692,
    0x5a42,
    0x5a42,
    0x5a32,
    0x5a32,
    0x53a2,
    0x53a2,
    0x5952,
    0x5952,
    0x5592,
    0x5592,
    0x4a22,
    0x4a22,
    0x4a22,
    0x4a22,
    0x42a2,
    0x42a2,
    0x42a2,
    0x42a2,
    0xf005,
    0x4a12,
    0x4a12,
    0x41a2,
    0x41a2,
    0x5a01,
    0x5862,
    0x40a1,
    0x40a1,
    0x5682,
    0x5942,
    0x4392,
    0x4392,
    0x5932,
    0x5852,
    0x5582,
    0x5762,
    0x4922,
    0x4922,
    0x4292,
    0x4292,
    0x5752,
    0x5572,
    0x4832,
    0x4832,
    0x4382,
    0x4382,
    0x5662,
    0x5742,
    0x5472,
    0x5652,
    0x5562,
    0x5372,
    0xf005,
    0x3912,
    0x3912,
    0x3912,
    0x3912,
    0x3192,
    0x3192,
    0x3192,
    0x3192,
    0x4901,
    0x4901,
    0x4091,
    0x4091,
    0x4842,
    0x4842,
    0x4482,
    0x4482,
    0x4272,
    0x4272,
    0x5642,
    0x5462,
    0x3822,
    0x3822,
    0x3822,
    0x3822,
    0x3282,
    0x3282,
    0x3282,
    0x3282,
    0x3812,
    0x3812,
    0x3812,
    0x3812,
    0xf004,
    0x4732,
    0x4722,
    0x3712,
    0x3712,
    0x3172,
    0x3172,
    0x4552,
    0x4701,
    0x4071,
    0x4632,
    0x4362,
    0x4542,
    0x4452,
    0x4622,
    0x4262,
    0x4532,
    0xf003,
    0x2182,
    0x2182,
    0x3801,
    0x3081,
    0x3612,
    0x3162,
    0x3601,
    0x3061,
    0xf004,
    0x4352,
    0x4442,
    0x3522,
    0x3522,
    0x3252,
    0x3252,
    0x3501,
    0x3501,
    0x2512,
    0x2512,
    0x2512,
    0x2512,
    0x2152,
    0x2152,
    0x2152,
    0x2152,
    0xf003,
    0x3432,
    0x3342,
    0x3051,
    0x3422,
    0x3242,
    0x3332,
    0x2412,
    0x2412,
    0xf002,
    0x1142,
    0x1142,
    0x2401,
    0x2041,
    0xf002,
    0x2322,
    0x2232,
    0x1312,
    0x1312,
    0xf001,
    0x1132,
    0x1301,
    0xf001,
    0x1031,
    0x1222,
    0xf003,
    0x0082,
    0x008b,
    0x008e,
    0x0091,
    0x0094,
    0x0097,
    0x3ce2,
    0x3dd2,
    0xf003,
    0x0093,
    0x3eb2,
    0x3be2,
    0x3f92,
    0x39f2,
    0x3ae2,
    0x3db2,
    0x3bd2,
    0xf003,
    0x3f82,
    0x38f2,
    0x3cc2,
    0x008d,
    0x3e82,
    0x0090,
    0x27f2,
    0x27f2,
    0xf003,
    0x2ad2,
    0x2ad2,
    0x3da2,
    0x3cb2,
    0x3bc2,
    0x36f2,
    0x2f62,
    0x2f62,
    0xf002,
    0x28e2,
    0x2f52,
    0x2d92,
    0x29d2,
    0xf002,
    0x25f2,
    0x27e2,
    0x2ca2,
    0x2bb2,
    0xf003,
    0x2f42,
    0x2f42,
    0x24f2,
    0x24f2,
    0x3ac2,
    0x36e2,
    0x23f2,
    0x23f2,
    0xf002,
    0x1f32,
    0x1f32,
    0x2d82,
    0x28d2,
    0xf001,
    0x1f22,
    0x12f2,
    0xf002,
    0x2e62,
    0x2c92,
    0x1f01,
    0x1f01,
    0xf002,
    0x29c2,
    0x2e52,
    0x1ba2,
    0x1ba2,
    0xf002,
    0x2d72,
    0x27d2,
    0x1e42,
    0x1e42,
    0xf002,
    0x28c2,
    0x26d2,
    0x1e32,
    0x1e32,
    0xf002,
    0x19b2,
    0x19b2,
    0x2b92,
    0x2aa2,
    0xf001,
    0x1ab2,
    0x15e2,
    0xf001,
    0x14e2,
    0x1c82,
    0xf001,
    0x1d62,
    0x13e2,
    0xf001,
    0x1e22,
    0x1e01,
    0xf001,
    0x10e1,
    0x1d52,
    0xf001,
    0x15d2,
    0x1c72,
    0xf001,
    0x17c2,
    0x1d42,
    0xf001,
    0x1b82,
    0x18b2,
    0xf001,
    0x14d2,
    0x1a92,
    0xf001,
    0x19a2,
    0x1c62,
    0xf001,
    0x13d2,
    0x1b72,
    0xf001,
    0x1c52,
    0x15c2,
    0xf001,
    0x1992,
    0x1a72,
    0xf001,
    0x17a2,
    0x1792,
    0xf003,
    0x0023,
    0x3df2,
    0x2de2,
    0x2de2,
    0x1ff2,
    0x1ff2,
    0x1ff2,
    0x1ff2,
    0xf001,
    0x1fe2,
    0x1fd2,
    0xf001,
    0x1ee2,
    0x1fc2,
    0xf001,
    0x1ed2,
    0x1fb2,
    0xf001,
    0x1bf2,
    0x1ec2,
    0xf002,
    0x1cd2,
    0x1cd2,
    0x2fa2,
    0x29e2,
    0xf001,
    0x1af2,
    0x1dc2,
    0xf001,
    0x1ea2,
    0x1e92,
    0xf001,
    0x1f72,
    0x1e72,
    0xf001,
    0x1ef2,
    0x1cf2,

    /* huffTable15[580] */
    0xf008,
    0x0101,
    0x0122,
    0x0143,
    0x0154,
    0x0165,
    0x0176,
    0x017f,
    0x0188,
    0x0199,
    0x01a2,
    0x01ab,
    0x01b4,
    0x01bd,
    0x01c2,
    0x01cb,
    0x01d4,
    0x01d9,
    0x01de,
    0x01e3,
    0x01e8,
    0x01ed,
    0x01f2,
    0x01f7,
    0x01fc,
    0x0201,
    0x0204,
    0x0207,
    0x020a,
    0x020f,
    0x0212,
    0x0215,
    0x021a,
    0x021d,
    0x0220,
    0x8192,
    0x0223,
    0x0226,
    0x0229,
    0x022c,
    0x022f,
    0x8822,
    0x8282,
    0x8812,
    0x8182,
    0x0232,
    0x0235,
    0x0238,
    0x023b,
    0x8722,
    0x8272,
    0x8462,
    0x8712,
    0x8552,
    0x8172,
    0x023e,
    0x8632,
    0x8362,
    0x8542,
    0x8452,
    0x8622,
    0x8262,
    0x8612,
    0x0241,
    0x8532,
    0x7162,
    0x7162,
    0x8352,
    0x8442,
    0x7522,
    0x7522,
    0x7252,
    0x7252,
    0x7512,
    0x7512,
    0x7152,
    0x7152,
    0x8501,
    0x8051,
    0x7432,
    0x7432,
    0x7342,
    0x7342,
    0x7422,
    0x7422,
    0x7242,
    0x7242,
    0x7332,
    0x7332,
    0x6142,
    0x6142,
    0x6142,
    0x6142,
    0x7412,
    0x7412,
    0x7401,
    0x7401,
    0x6322,
    0x6322,
    0x6322,
    0x6322,
    0x6232,
    0x6232,
    0x6232,
    0x6232,
    0x7041,
    0x7041,
    0x7301,
    0x7301,
    0x6312,
    0x6312,
    0x6312,
    0x6312,
    0x6132,
    0x6132,
    0x6132,
    0x6132,
    0x6031,
    0x6031,
    0x6031,
    0x6031,
    0x5222,
    0x5222,
    0x5222,
    0x5222,
    0x5222,
    0x5222,
    0x5222,
    0x5222,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5201,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x5021,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x3112,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0x3000,
    0xf005,
    0x5ff2,
    0x5fe2,
    0x5ef2,
    0x5fd2,
    0x4ee2,
    0x4ee2,
    0x5df2,
    0x5fc2,
    0x5cf2,
    0x5ed2,
    0x5de2,
    0x5fb2,
    0x4bf2,
    0x4bf2,
    0x5ec2,
    0x5ce2,
    0x4dd2,
    0x4dd2,
    0x4fa2,
    0x4fa2,
    0x4af2,
    0x4af2,
    0x4eb2,
    0x4eb2,
    0x4be2,
    0x4be2,
    0x4dc2,
    0x4dc2,
    0x4cd2,
    0x4cd2,
    0x4f92,
    0x4f92,
    0xf005,
    0x49f2,
    0x49f2,
    0x4ae2,
    0x4ae2,
    0x4db2,
    0x4db2,
    0x4bd2,
    0x4bd2,
    0x4f82,
    0x4f82,
    0x48f2,
    0x48f2,
    0x4cc2,
    0x4cc2,
    0x4e92,
    0x4e92,
    0x49e2,
    0x49e2,
    0x4f72,
    0x4f72,
    0x47f2,
    0x47f2,
    0x4da2,
    0x4da2,
    0x4ad2,
    0x4ad2,
    0x4cb2,
    0x4cb2,
    0x4f62,
    0x4f62,
    0x5ea2,
    0x5f01,
    0xf004,
    0x3bc2,
    0x3bc2,
    0x36f2,
    0x36f2,
    0x4e82,
    0x48e2,
    0x4f52,
    0x4d92,
    0x35f2,
    0x35f2,
    0x3e72,
    0x3e72,
    0x37e2,
    0x37e2,
    0x3ca2,
    0x3ca2,
    0xf004,
    0x3ac2,
    0x3ac2,
    0x3bb2,
    0x3bb2,
    0x49d2,
    0x4d82,
    0x3f42,
    0x3f42,
    0x34f2,
    0x34f2,
    0x3f32,
    0x3f32,
    0x33f2,
    0x33f2,
    0x38d2,
    0x38d2,
    0xf004,
    0x36e2,
    0x36e2,
    0x3f22,
    0x3f22,
    0x32f2,
    0x32f2,
    0x4e62,
    0x40f1,
    0x3f12,
    0x3f12,
    0x31f2,
    0x31f2,
    0x3c92,
    0x3c92,
    0x39c2,
    0x39c2,
    0xf003,
    0x3e52,
    0x3ba2,
    0x3ab2,
    0x35e2,
    0x3d72,
    0x37d2,
    0x3e42,
    0x34e2,
    0xf003,
    0x3c82,
    0x38c2,
    0x3e32,
    0x3d62,
    0x36d2,
    0x33e2,
    0x3b92,
    0x39b2,
    0xf004,
    0x3e22,
    0x3e22,
    0x3aa2,
    0x3aa2,
    0x32e2,
    0x32e2,
    0x3e12,
    0x3e12,
    0x31e2,
    0x31e2,
    0x4e01,
    0x40e1,
    0x3d52,
    0x3d52,
    0x35d2,
    0x35d2,
    0xf003,
    0x3c72,
    0x37c2,
    0x3d42,
    0x3b82,
    0x24d2,
    0x24d2,
    0x38b2,
    0x3a92,
    0xf003,
    0x39a2,
    0x3c62,
    0x36c2,
    0x3d32,
    0x23d2,
    0x23d2,
    0x22d2,
    0x22d2,
    0xf003,
    0x3d22,
    0x3d01,
    0x2d12,
    0x2d12,
    0x2b72,
    0x2b72,
    0x27b2,
    0x27b2,
    0xf003,
    0x21d2,
    0x21d2,
    0x3c52,
    0x30d1,
    0x25c2,
    0x25c2,
    0x2a82,
    0x2a82,
    0xf002,
    0x28a2,
    0x2c42,
    0x24c2,
    0x2b62,
    0xf003,
    0x26b2,
    0x26b2,
    0x3992,
    0x3c01,
    0x2c32,
    0x2c32,
    0x23c2,
    0x23c2,
    0xf003,
    0x2a72,
    0x2a72,
    0x27a2,
    0x27a2,
    0x26a2,
    0x26a2,
    0x30c1,
    0x3b01,
    0xf002,
    0x12c2,
    0x12c2,
    0x2c22,
    0x2b52,
    0xf002,
    0x25b2,
    0x2c12,
    0x2982,
    0x2892,
    0xf002,
    0x21c2,
    0x2b42,
    0x24b2,
    0x2a62,
    0xf002,
    0x2b32,
    0x2972,
    0x13b2,
    0x13b2,
    0xf002,
    0x2792,
    0x2882,
    0x2b22,
    0x2a52,
    0xf002,
    0x12b2,
    0x12b2,
    0x25a2,
    0x2b12,
    0xf002,
    0x11b2,
    0x11b2,
    0x20b1,
    0x2962,
    0xf002,
    0x2692,
    0x2a42,
    0x24a2,
    0x2872,
    0xf002,
    0x2782,
    0x2a32,
    0x13a2,
    0x13a2,
    0xf001,
    0x1952,
    0x1592,
    0xf001,
    0x1a22,
    0x12a2,
    0xf001,
    0x1a12,
    0x11a2,
    0xf002,
    0x2a01,
    0x20a1,
    0x1862,
    0x1862,
    0xf001,
    0x1682,
    0x1942,
    0xf001,
    0x1492,
    0x1932,
    0xf002,
    0x1392,
    0x1392,
    0x2772,
    0x2901,
    0xf001,
    0x1852,
    0x1582,
    0xf001,
    0x1922,
    0x1762,
    0xf001,
    0x1672,
    0x1292,
    0xf001,
    0x1912,
    0x1091,
    0xf001,
    0x1842,
    0x1482,
    0xf001,
    0x1752,
    0x1572,
    0xf001,
    0x1832,
    0x1382,
    0xf001,
    0x1662,
    0x1742,
    0xf001,
    0x1472,
    0x1801,
    0xf001,
    0x1081,
    0x1652,
    0xf001,
    0x1562,
    0x1732,
    0xf001,
    0x1372,
    0x1642,
    0xf001,
    0x1701,
    0x1071,
    0xf001,
    0x1601,
    0x1061,

    /* huffTable16[651] */
    0xf008,
    0x0101,
    0x010a,
    0x0113,
    0x8ff2,
    0x0118,
    0x011d,
    0x0120,
    0x82f2,
    0x0131,
    0x8f12,
    0x81f2,
    0x0134,
    0x0145,
    0x0156,
    0x0167,
    0x0178,
    0x0189,
    0x019a,
    0x01a3,
    0x01ac,
    0x01b5,
    0x01be,
    0x01c7,
    0x01d0,
    0x01d9,
    0x01de,
    0x01e3,
    0x01e6,
    0x01eb,
    0x01f0,
    0x8152,
    0x01f3,
    0x01f6,
    0x01f9,
    0x01fc,
    0x8412,
    0x8142,
    0x01ff,
    0x8322,
    0x8232,
    0x7312,
    0x7312,
    0x7132,
    0x7132,
    0x8301,
    0x8031,
    0x7222,
    0x7222,
    0x6212,
    0x6212,
    0x6212,
    0x6212,
    0x6122,
    0x6122,
    0x6122,
    0x6122,
    0x6201,
    0x6201,
    0x6201,
    0x6201,
    0x6021,
    0x6021,
    0x6021,
    0x6021,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x3011,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0x1000,
    0xf003,
    0x3fe2,
    0x3ef2,
    0x3fd2,
    0x3df2,
    0x3fc2,
    0x3cf2,
    0x3fb2,
    0x3bf2,
    0xf003,
    0x2fa2,
    0x2fa2,
    0x3af2,
    0x3f92,
    0x39f2,
    0x38f2,
    0x2f82,
    0x2f82,
    0xf002,
    0x2f72,
    0x27f2,
    0x2f62,
    0x26f2,
    0xf002,
    0x2f52,
    0x25f2,
    0x1f42,
    0x1f42,
    0xf001,
    0x14f2,
    0x13f2,
    0xf004,
    0x10f1,
    0x10f1,
    0x10f1,
    0x10f1,
    0x10f1,
    0x10f1,
    0x10f1,
    0x10f1,
    0x2f32,
    0x2f32,
    0x2f32,
    0x2f32,
    0x00e2,
    0x00f3,
    0x00fc,
    0x0105,
    0xf001,
    0x1f22,
    0x1f01,
    0xf004,
    0x00fa,
    0x00ff,
    0x0104,
    0x0109,
    0x010c,
    0x0111,
    0x0116,
    0x0119,
    0x011e,
    0x0123,
    0x0128,
    0x43e2,
    0x012d,
    0x0130,
    0x0133,
    0x0136,
    0xf004,
    0x0128,
    0x012b,
    0x012e,
    0x4d01,
    0x0131,
    0x0134,
    0x0137,
    0x4c32,
    0x013a,
    0x4c12,
    0x40c1,
    0x013d,
    0x32e2,
    0x32e2,
    0x4e22,
    0x4e12,
    0xf004,
    0x43d2,
    0x4d22,
    0x42d2,
    0x41d2,
    0x4b32,
    0x012f,
    0x3d12,
    0x3d12,
    0x44c2,
    0x4b62,
    0x43c2,
    0x47a2,
    0x3c22,
    0x3c22,
    0x42c2,
    0x45b2,
    0xf004,
    0x41c2,
    0x4c01,
    0x4b42,
    0x44b2,
    0x4a62,
    0x46a2,
    0x33b2,
    0x33b2,
    0x4a52,
    0x45a2,
    0x3b22,
    0x3b22,
    0x32b2,
    0x32b2,
    0x3b12,
    0x3b12,
    0xf004,
    0x31b2,
    0x31b2,
    0x4b01,
    0x40b1,
    0x4962,
    0x4692,
    0x4a42,
    0x44a2,
    0x4872,
    0x4782,
    0x33a2,
    0x33a2,
    0x4a32,
    0x4952,
    0x3a22,
    0x3a22,
    0xf004,
    0x4592,
    0x4862,
    0x31a2,
    0x31a2,
    0x4682,
    0x4772,
    0x3492,
    0x3492,
    0x4942,
    0x4752,
    0x3762,
    0x3762,
    0x22a2,
    0x22a2,
    0x22a2,
    0x22a2,
    0xf003,
    0x2a12,
    0x2a12,
    0x3a01,
    0x30a1,
    0x3932,
    0x3392,
    0x3852,
    0x3582,
    0xf003,
    0x2922,
    0x2922,
    0x2292,
    0x2292,
    0x3672,
    0x3901,
    0x2912,
    0x2912,
    0xf003,
    0x2192,
    0x2192,
    0x3091,
    0x3842,
    0x3482,
    0x3572,
    0x3832,
    0x3382,
    0xf003,
    0x3662,
    0x3822,
    0x2282,
    0x2282,
    0x3742,
    0x3472,
    0x2812,
    0x2812,
    0xf003,
    0x2182,
    0x2182,
    0x2081,
    0x2081,
    0x3801,
    0x3652,
    0x2732,
    0x2732,
    0xf003,
    0x2372,
    0x2372,
    0x3562,
    0x3642,
    0x2722,
    0x2722,
    0x2272,
    0x2272,
    0xf003,
    0x3462,
    0x3552,
    0x2701,
    0x2701,
    0x1712,
    0x1712,
    0x1712,
    0x1712,
    0xf002,
    0x1172,
    0x1172,
    0x2071,
    0x2632,
    0xf002,
    0x2362,
    0x2542,
    0x2452,
    0x2622,
    0xf001,
    0x1262,
    0x1612,
    0xf002,
    0x1162,
    0x1162,
    0x2601,
    0x2061,
    0xf002,
    0x1352,
    0x1352,
    0x2532,
    0x2442,
    0xf001,
    0x1522,
    0x1252,
    0xf001,
    0x1512,
    0x1501,
    0xf001,
    0x1432,
    0x1342,
    0xf001,
    0x1051,
    0x1422,
    0xf001,
    0x1242,
    0x1332,
    0xf001,
    0x1401,
    0x1041,
    0xf004,
    0x4ec2,
    0x0086,
    0x3ed2,
    0x3ed2,
    0x39e2,
    0x39e2,
    0x4ae2,
    0x49d2,
    0x2ee2,
    0x2ee2,
    0x2ee2,
    0x2ee2,
    0x3de2,
    0x3de2,
    0x3be2,
    0x3be2,
    0xf003,
    0x2eb2,
    0x2eb2,
    0x2dc2,
    0x2dc2,
    0x3cd2,
    0x3bd2,
    0x2ea2,
    0x2ea2,
    0xf003,
    0x2cc2,
    0x2cc2,
    0x3da2,
    0x3ad2,
    0x3e72,
    0x3ca2,
    0x2ac2,
    0x2ac2,
    0xf003,
    0x39c2,
    0x3d72,
    0x2e52,
    0x2e52,
    0x1db2,
    0x1db2,
    0x1db2,
    0x1db2,
    0xf002,
    0x1e92,
    0x1e92,
    0x2cb2,
    0x2bc2,
    0xf002,
    0x2e82,
    0x28e2,
    0x2d92,
    0x27e2,
    0xf002,
    0x2bb2,
    0x2d82,
    0x28d2,
    0x2e62,
    0xf001,
    0x16e2,
    0x1c92,
    0xf002,
    0x2ba2,
    0x2ab2,
    0x25e2,
    0x27d2,
    0xf002,
    0x1e42,
    0x1e42,
    0x24e2,
    0x2c82,
    0xf001,
    0x18c2,
    0x1e32,
    0xf002,
    0x1d62,
    0x1d62,
    0x26d2,
    0x2b92,
    0xf002,
    0x29b2,
    0x2aa2,
    0x11e2,
    0x11e2,
    0xf002,
    0x14d2,
    0x14d2,
    0x28b2,
    0x29a2,
    0xf002,
    0x1b72,
    0x1b72,
    0x27b2,
    0x20d1,
    0xf001,
    0x1e01,
    0x10e1,
    0xf001,
    0x1d52,
    0x15d2,
    0xf001,
    0x1c72,
    0x17c2,
    0xf001,
    0x1d42,
    0x1b82,
    0xf001,
    0x1a92,
    0x1c62,
    0xf001,
    0x16c2,
    0x1d32,
    0xf001,
    0x1c52,
    0x15c2,
    0xf001,
    0x1a82,
    0x18a2,
    0xf001,
    0x1992,
    0x1c42,
    0xf001,
    0x16b2,
    0x1a72,
    0xf001,
    0x1b52,
    0x1982,
    0xf001,
    0x1892,
    0x1972,
    0xf001,
    0x1792,
    0x1882,
    0xf001,
    0x1ce2,
    0x1dd2,

    /* huffTable24[705] */
    0xf009,
    0x8fe2,
    0x8fe2,
    0x8ef2,
    0x8ef2,
    0x8fd2,
    0x8fd2,
    0x8df2,
    0x8df2,
    0x8fc2,
    0x8fc2,
    0x8cf2,
    0x8cf2,
    0x8fb2,
    0x8fb2,
    0x8bf2,
    0x8bf2,
    0x7af2,
    0x7af2,
    0x7af2,
    0x7af2,
    0x8fa2,
    0x8fa2,
    0x8f92,
    0x8f92,
    0x79f2,
    0x79f2,
    0x79f2,
    0x79f2,
    0x78f2,
    0x78f2,
    0x78f2,
    0x78f2,
    0x8f82,
    0x8f82,
    0x8f72,
    0x8f72,
    0x77f2,
    0x77f2,
    0x77f2,
    0x77f2,
    0x7f62,
    0x7f62,
    0x7f62,
    0x7f62,
    0x76f2,
    0x76f2,
    0x76f2,
    0x76f2,
    0x7f52,
    0x7f52,
    0x7f52,
    0x7f52,
    0x75f2,
    0x75f2,
    0x75f2,
    0x75f2,
    0x7f42,
    0x7f42,
    0x7f42,
    0x7f42,
    0x74f2,
    0x74f2,
    0x74f2,
    0x74f2,
    0x7f32,
    0x7f32,
    0x7f32,
    0x7f32,
    0x73f2,
    0x73f2,
    0x73f2,
    0x73f2,
    0x7f22,
    0x7f22,
    0x7f22,
    0x7f22,
    0x72f2,
    0x72f2,
    0x72f2,
    0x72f2,
    0x71f2,
    0x71f2,
    0x71f2,
    0x71f2,
    0x8f12,
    0x8f12,
    0x80f1,
    0x80f1,
    0x9f01,
    0x0201,
    0x0206,
    0x020b,
    0x0210,
    0x0215,
    0x021a,
    0x021f,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x4ff2,
    0x0224,
    0x0229,
    0x0232,
    0x0237,
    0x023a,
    0x023f,
    0x0242,
    0x0245,
    0x024a,
    0x024d,
    0x0250,
    0x0253,
    0x0256,
    0x0259,
    0x025c,
    0x025f,
    0x0262,
    0x0265,
    0x0268,
    0x026b,
    0x026e,
    0x0271,
    0x0274,
    0x0277,
    0x027a,
    0x027d,
    0x0280,
    0x0283,
    0x0288,
    0x028b,
    0x028e,
    0x0291,
    0x0294,
    0x0297,
    0x029a,
    0x029f,
    0x94b2,
    0x02a4,
    0x02a7,
    0x02aa,
    0x93b2,
    0x9882,
    0x02af,
    0x92b2,
    0x02b2,
    0x02b5,
    0x9692,
    0x94a2,
    0x02b8,
    0x9782,
    0x9a32,
    0x93a2,
    0x9952,
    0x9592,
    0x9a22,
    0x92a2,
    0x91a2,
    0x9862,
    0x9682,
    0x9772,
    0x9942,
    0x9492,
    0x9932,
    0x9392,
    0x9852,
    0x9582,
    0x9922,
    0x9762,
    0x9672,
    0x9292,
    0x9912,
    0x9192,
    0x9842,
    0x9482,
    0x9752,
    0x9572,
    0x9832,
    0x9382,
    0x9662,
    0x9822,
    0x9282,
    0x9812,
    0x9742,
    0x9472,
    0x9182,
    0x02bb,
    0x9652,
    0x9562,
    0x9712,
    0x02be,
    0x8372,
    0x8372,
    0x9732,
    0x9722,
    0x8272,
    0x8272,
    0x8642,
    0x8642,
    0x8462,
    0x8462,
    0x8552,
    0x8552,
    0x8172,
    0x8172,
    0x8632,
    0x8632,
    0x8362,
    0x8362,
    0x8542,
    0x8542,
    0x8452,
    0x8452,
    0x8622,
    0x8622,
    0x8262,
    0x8262,
    0x8612,
    0x8612,
    0x8162,
    0x8162,
    0x9601,
    0x9061,
    0x8532,
    0x8532,
    0x8352,
    0x8352,
    0x8442,
    0x8442,
    0x8522,
    0x8522,
    0x8252,
    0x8252,
    0x8512,
    0x8512,
    0x9501,
    0x9051,
    0x7152,
    0x7152,
    0x7152,
    0x7152,
    0x8432,
    0x8432,
    0x8342,
    0x8342,
    0x7422,
    0x7422,
    0x7422,
    0x7422,
    0x7242,
    0x7242,
    0x7242,
    0x7242,
    0x7332,
    0x7332,
    0x7332,
    0x7332,
    0x7412,
    0x7412,
    0x7412,
    0x7412,
    0x7142,
    0x7142,
    0x7142,
    0x7142,
    0x8401,
    0x8401,
    0x8041,
    0x8041,
    0x7322,
    0x7322,
    0x7322,
    0x7322,
    0x7232,
    0x7232,
    0x7232,
    0x7232,
    0x6312,
    0x6312,
    0x6312,
    0x6312,
    0x6312,
    0x6312,
    0x6312,
    0x6312,
    0x6132,
    0x6132,
    0x6132,
    0x6132,
    0x6132,
    0x6132,
    0x6132,
    0x6132,
    0x7301,
    0x7301,
    0x7301,
    0x7301,
    0x7031,
    0x7031,
    0x7031,
    0x7031,
    0x6222,
    0x6222,
    0x6222,
    0x6222,
    0x6222,
    0x6222,
    0x6222,
    0x6222,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5212,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x5122,
    0x6201,
    0x6201,
    0x6201,
    0x6201,
    0x6201,
    0x6201,
    0x6201,
    0x6201,
    0x6021,
    0x6021,
    0x6021,
    0x6021,
    0x6021,
    0x6021,
    0x6021,
    0x6021,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4112,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4101,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4011,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0x4000,
    0xf002,
    0x2ee2,
    0x2ed2,
    0x2de2,
    0x2ec2,
    0xf002,
    0x2ce2,
    0x2dd2,
    0x2eb2,
    0x2be2,
    0xf002,
    0x2dc2,
    0x2cd2,
    0x2ea2,
    0x2ae2,
    0xf002,
    0x2db2,
    0x2bd2,
    0x2cc2,
    0x2e92,
    0xf002,
    0x29e2,
    0x2da2,
    0x2ad2,
    0x2cb2,
    0xf002,
    0x2bc2,
    0x2e82,
    0x28e2,
    0x2d92,
    0xf002,
    0x29d2,
    0x2e72,
    0x27e2,
    0x2ca2,
    0xf002,
    0x2ac2,
    0x2bb2,
    0x2d82,
    0x28d2,
    0xf003,
    0x3e01,
    0x30e1,
    0x2d01,
    0x2d01,
    0x16e2,
    0x16e2,
    0x16e2,
    0x16e2,
    0xf002,
    0x2e62,
    0x2c92,
    0x19c2,
    0x19c2,
    0xf001,
    0x1e52,
    0x1ab2,
    0xf002,
    0x15e2,
    0x15e2,
    0x2ba2,
    0x2d72,
    0xf001,
    0x17d2,
    0x14e2,
    0xf001,
    0x1c82,
    0x18c2,
    0xf002,
    0x2e42,
    0x2e22,
    0x1e32,
    0x1e32,
    0xf001,
    0x1d62,
    0x16d2,
    0xf001,
    0x13e2,
    0x1b92,
    0xf001,
    0x19b2,
    0x1aa2,
    0xf001,
    0x12e2,
    0x1e12,
    0xf001,
    0x11e2,
    0x1d52,
    0xf001,
    0x15d2,
    0x1c72,
    0xf001,
    0x17c2,
    0x1d42,
    0xf001,
    0x1b82,
    0x18b2,
    0xf001,
    0x14d2,
    0x1a92,
    0xf001,
    0x19a2,
    0x1c62,
    0xf001,
    0x16c2,
    0x1d32,
    0xf001,
    0x13d2,
    0x1d22,
    0xf001,
    0x12d2,
    0x1d12,
    0xf001,
    0x1b72,
    0x17b2,
    0xf001,
    0x11d2,
    0x1c52,
    0xf001,
    0x15c2,
    0x1a82,
    0xf001,
    0x18a2,
    0x1992,
    0xf001,
    0x1c42,
    0x14c2,
    0xf001,
    0x1b62,
    0x16b2,
    0xf002,
    0x20d1,
    0x2c01,
    0x1c32,
    0x1c32,
    0xf001,
    0x13c2,
    0x1a72,
    0xf001,
    0x17a2,
    0x1c22,
    0xf001,
    0x12c2,
    0x1b52,
    0xf001,
    0x15b2,
    0x1c12,
    0xf001,
    0x1982,
    0x1892,
    0xf001,
    0x11c2,
    0x1b42,
    0xf002,
    0x20c1,
    0x2b01,
    0x1b32,
    0x1b32,
    0xf002,
    0x20b1,
    0x2a01,
    0x1a12,
    0x1a12,
    0xf001,
    0x1a62,
    0x16a2,
    0xf001,
    0x1972,
    0x1792,
    0xf002,
    0x20a1,
    0x2901,
    0x1091,
    0x1091,
    0xf001,
    0x1b22,
    0x1a52,
    0xf001,
    0x15a2,
    0x1b12,
    0xf001,
    0x11b2,
    0x1962,
    0xf001,
    0x1a42,
    0x1872,
    0xf001,
    0x1801,
    0x1081,
    0xf001,
    0x1701,
    0x1071,
};

#define HUFF_OFFSET_01 0
#define HUFF_OFFSET_02 (9 + HUFF_OFFSET_01)
#define HUFF_OFFSET_03 (65 + HUFF_OFFSET_02)
#define HUFF_OFFSET_05 (65 + HUFF_OFFSET_03)
#define HUFF_OFFSET_06 (257 + HUFF_OFFSET_05)
#define HUFF_OFFSET_07 (129 + HUFF_OFFSET_06)
#define HUFF_OFFSET_08 (110 + HUFF_OFFSET_07)
#define HUFF_OFFSET_09 (280 + HUFF_OFFSET_08)
#define HUFF_OFFSET_10 (93 + HUFF_OFFSET_09)
#define HUFF_OFFSET_11 (320 + HUFF_OFFSET_10)
#define HUFF_OFFSET_12 (296 + HUFF_OFFSET_11)
#define HUFF_OFFSET_13 (185 + HUFF_OFFSET_12)
#define HUFF_OFFSET_15 (497 + HUFF_OFFSET_13)
#define HUFF_OFFSET_16 (580 + HUFF_OFFSET_15)
#define HUFF_OFFSET_24 (651 + HUFF_OFFSET_16)

const int huffTabOffset[HUFF_PAIRTABS] = {
    0,
    HUFF_OFFSET_01,
    HUFF_OFFSET_02,
    HUFF_OFFSET_03,
    0,
    HUFF_OFFSET_05,
    HUFF_OFFSET_06,
    HUFF_OFFSET_07,
    HUFF_OFFSET_08,
    HUFF_OFFSET_09,
    HUFF_OFFSET_10,
    HUFF_OFFSET_11,
    HUFF_OFFSET_12,
    HUFF_OFFSET_13,
    0,
    HUFF_OFFSET_15,
    HUFF_OFFSET_16,
    HUFF_OFFSET_16,
    HUFF_OFFSET_16,
    HUFF_OFFSET_16,
    HUFF_OFFSET_16,
    HUFF_OFFSET_16,
    HUFF_OFFSET_16,
    HUFF_OFFSET_16,
    HUFF_OFFSET_24,
    HUFF_OFFSET_24,
    HUFF_OFFSET_24,
    HUFF_OFFSET_24,
    HUFF_OFFSET_24,
    HUFF_OFFSET_24,
    HUFF_OFFSET_24,
    HUFF_OFFSET_24,
};

const HuffTabLookup huffTabLookup[HUFF_PAIRTABS] = {
    {0, noBits},        {0, oneShot},       {0, oneShot},       {0, oneShot},       {0, invalidTab},
    {0, oneShot},       {0, oneShot},       {0, loopNoLinbits}, {0, loopNoLinbits}, {0, loopNoLinbits},
    {0, loopNoLinbits}, {0, loopNoLinbits}, {0, loopNoLinbits}, {0, loopNoLinbits}, {0, invalidTab},
    {0, loopNoLinbits}, {1, loopLinbits},   {2, loopLinbits},   {3, loopLinbits},   {4, loopLinbits},
    {6, loopLinbits},   {8, loopLinbits},   {10, loopLinbits},  {13, loopLinbits},  {4, loopLinbits},
    {5, loopLinbits},   {6, loopLinbits},   {7, loopLinbits},   {8, loopLinbits},   {9, loopLinbits},
    {11, loopLinbits},  {13, loopLinbits},
};

/* tables for quadruples
 * format 0xAB
 *  A = length of codeword
 *  B = codeword
 */
const unsigned char quadTable[64 + 16] = {
    /* table A */
    0x6b,
    0x6f,
    0x6d,
    0x6e,
    0x67,
    0x65,
    0x59,
    0x59,
    0x56,
    0x56,
    0x53,
    0x53,
    0x5a,
    0x5a,
    0x5c,
    0x5c,
    0x42,
    0x42,
    0x42,
    0x42,
    0x41,
    0x41,
    0x41,
    0x41,
    0x44,
    0x44,
    0x44,
    0x44,
    0x48,
    0x48,
    0x48,
    0x48,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    0x10,
    /* table B */
    0x4f,
    0x4e,
    0x4d,
    0x4c,
    0x4b,
    0x4a,
    0x49,
    0x48,
    0x47,
    0x46,
    0x45,
    0x44,
    0x43,
    0x42,
    0x41,
    0x40,
};

const int quadTabOffset[2] = {0, 64};
const int quadTabMaxBits[2] = {6, 4};

/* helper macros - see comments in hufftabs.c about the format of the huffman
 * tables */
#define GetMaxbits(x) ((int) ((((unsigned short) (x)) >> 0) & 0x000f))
#define GetHLen(x) ((int) ((((unsigned short) (x)) >> 12) & 0x000f))
#define GetCWY(x) ((int) ((((unsigned short) (x)) >> 8) & 0x000f))
#define GetCWX(x) ((int) ((((unsigned short) (x)) >> 4) & 0x000f))
#define GetSignBits(x) ((int) ((((unsigned short) (x)) >> 0) & 0x000f))

#define GetHLenQ(x) ((int) ((((unsigned char) (x)) >> 4) & 0x0f))
#define GetCWVQ(x) ((int) ((((unsigned char) (x)) >> 3) & 0x01))
#define GetCWWQ(x) ((int) ((((unsigned char) (x)) >> 2) & 0x01))
#define GetCWXQ(x) ((int) ((((unsigned char) (x)) >> 1) & 0x01))
#define GetCWYQ(x) ((int) ((((unsigned char) (x)) >> 0) & 0x01))

/* apply sign of s to the positive number x (save in MSB, will do two's
 * complement in dequant) */
#define ApplySign(x, s) \
  { (x) |= ((s) & 0x80000000); }

/**************************************************************************************
 * Function:    DecodeHuffmanPairs
 *
 * Description: decode 2-way vector Huffman codes in the "bigValues" region of
 *spectrum
 *
 * Inputs:      valid BitStreamInfo struct, pointing to start of pair-wise codes
 *              pointer to xy buffer to received decoded values
 *              number of codewords to decode
 *              index of Huffman table to use
 *              number of bits remaining in bitstream
 *
 * Outputs:     pairs of decoded coefficients in vwxy
 *              updated BitStreamInfo struct
 *
 * Return:      number of bits used, or -1 if out of bits
 *
 * Notes:       assumes that nVals is an even number
 *              si_huff.bit tests every Huffman codeword in every table (though
 *not necessarily all linBits outputs for x,y > 15)
 **************************************************************************************/
// no improvement with section=data
static int DecodeHuffmanPairs(int *xy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset) {
  int i, x, y;
  int cachedBits, padBits, len, startBits, linBits, maxBits, minBits;
  HuffTabType tabType;
  unsigned short cw, *tBase, *tCurr;
  unsigned int cache;

  if (nVals <= 0)
    return 0;

  if (bitsLeft < 0)
    return -1;
  startBits = bitsLeft;

  tBase = (unsigned short *) (huffTable + huffTabOffset[tabIdx]);
  linBits = huffTabLookup[tabIdx].linBits;
  tabType = huffTabLookup[tabIdx].tabType;

  ASSERT(!(nVals & 0x01));
  ASSERT(tabIdx < HUFF_PAIRTABS);
  ASSERT(tabIdx >= 0);
  ASSERT(tabType != invalidTab);

  /* initially fill cache with any partial byte */
  cache = 0;
  cachedBits = (8 - bitOffset) & 0x07;
  if (cachedBits)
    cache = (unsigned int) (*buf++) << (32 - cachedBits);
  bitsLeft -= cachedBits;

  if (tabType == noBits) {
    /* table 0, no data, x = y = 0 */
    for (i = 0; i < nVals; i += 2) {
      xy[i + 0] = 0;
      xy[i + 1] = 0;
    }
    return 0;
  } else if (tabType == oneShot) {
    /* single lookup, no escapes */
    maxBits = GetMaxbits(tBase[0]);
    tBase++;
    padBits = 0;
    while (nVals > 0) {
      /* refill cache - assumes cachedBits <= 16 */
      if (bitsLeft >= 16) {
        /* load 2 new bytes into left-justified cache */
        cache |= (unsigned int) (*buf++) << (24 - cachedBits);
        cache |= (unsigned int) (*buf++) << (16 - cachedBits);
        cachedBits += 16;
        bitsLeft -= 16;
      } else {
        /* last time through, pad cache with zeros and drain cache */
        if (cachedBits + bitsLeft <= 0)
          return -1;
        if (bitsLeft > 0)
          cache |= (unsigned int) (*buf++) << (24 - cachedBits);
        if (bitsLeft > 8)
          cache |= (unsigned int) (*buf++) << (16 - cachedBits);
        cachedBits += bitsLeft;
        bitsLeft = 0;

        cache &= (signed int) 0x80000000 >> (cachedBits - 1);
        padBits = 11;
        cachedBits += padBits; /* okay if this is > 32 (0's automatically
                                  shifted in from right) */
      }

      /* largest maxBits = 9, plus 2 for sign bits, so make sure cache has at
       * least 11 bits */
      while (nVals > 0 && cachedBits >= 11) {
        cw = tBase[cache >> (32 - maxBits)];
        len = GetHLen(cw);
        cachedBits -= len;
        cache <<= len;

        x = GetCWX(cw);
        if (x) {
          ApplySign(x, cache);
          cache <<= 1;
          cachedBits--;
        }
        y = GetCWY(cw);
        if (y) {
          ApplySign(y, cache);
          cache <<= 1;
          cachedBits--;
        }

        /* ran out of bits - should never have consumed padBits */
        if (cachedBits < padBits)
          return -1;

        *xy++ = x;
        *xy++ = y;
        nVals -= 2;
      }
    }
    bitsLeft += (cachedBits - padBits);
    return (startBits - bitsLeft);
  } else if (tabType == loopLinbits || tabType == loopNoLinbits) {
    tCurr = tBase;
    padBits = 0;
    while (nVals > 0) {
      /* refill cache - assumes cachedBits <= 16 */
      if (bitsLeft >= 16) {
        /* load 2 new bytes into left-justified cache */
        cache |= (unsigned int) (*buf++) << (24 - cachedBits);
        cache |= (unsigned int) (*buf++) << (16 - cachedBits);
        cachedBits += 16;
        bitsLeft -= 16;
      } else {
        /* last time through, pad cache with zeros and drain cache */
        if (cachedBits + bitsLeft <= 0)
          return -1;
        if (bitsLeft > 0)
          cache |= (unsigned int) (*buf++) << (24 - cachedBits);
        if (bitsLeft > 8)
          cache |= (unsigned int) (*buf++) << (16 - cachedBits);
        cachedBits += bitsLeft;
        bitsLeft = 0;

        cache &= (signed int) 0x80000000 >> (cachedBits - 1);
        padBits = 11;
        cachedBits += padBits; /* okay if this is > 32 (0's automatically
                                  shifted in from right) */
      }

      /* largest maxBits = 9, plus 2 for sign bits, so make sure cache has at
       * least 11 bits */
      while (nVals > 0 && cachedBits >= 11) {
        maxBits = GetMaxbits(tCurr[0]);
        cw = tCurr[(cache >> (32 - maxBits)) + 1];
        len = GetHLen(cw);
        if (!len) {
          cachedBits -= maxBits;
          cache <<= maxBits;
          tCurr += cw;
          continue;
        }
        cachedBits -= len;
        cache <<= len;

        x = GetCWX(cw);
        y = GetCWY(cw);

        if (x == 15 && tabType == loopLinbits) {
          minBits = linBits + 1 + (y ? 1 : 0);
          if (cachedBits + bitsLeft < minBits)
            return -1;
          while (cachedBits < minBits) {
            cache |= (unsigned int) (*buf++) << (24 - cachedBits);
            cachedBits += 8;
            bitsLeft -= 8;
          }
          if (bitsLeft < 0) {
            cachedBits += bitsLeft;
            bitsLeft = 0;
            cache &= (signed int) 0x80000000 >> (cachedBits - 1);
          }
          x += (int) (cache >> (32 - linBits));
          cachedBits -= linBits;
          cache <<= linBits;
        }
        if (x) {
          ApplySign(x, cache);
          cache <<= 1;
          cachedBits--;
        }

        if (y == 15 && tabType == loopLinbits) {
          minBits = linBits + 1;
          if (cachedBits + bitsLeft < minBits)
            return -1;
          while (cachedBits < minBits) {
            cache |= (unsigned int) (*buf++) << (24 - cachedBits);
            cachedBits += 8;
            bitsLeft -= 8;
          }
          if (bitsLeft < 0) {
            cachedBits += bitsLeft;
            bitsLeft = 0;
            cache &= (signed int) 0x80000000 >> (cachedBits - 1);
          }
          y += (int) (cache >> (32 - linBits));
          cachedBits -= linBits;
          cache <<= linBits;
        }
        if (y) {
          ApplySign(y, cache);
          cache <<= 1;
          cachedBits--;
        }

        /* ran out of bits - should never have consumed padBits */
        if (cachedBits < padBits)
          return -1;

        *xy++ = x;
        *xy++ = y;
        nVals -= 2;
        tCurr = tBase;
      }
    }
    bitsLeft += (cachedBits - padBits);
    return (startBits - bitsLeft);
  }

  /* error in bitstream - trying to access unused Huffman table */
  return -1;
}

/**************************************************************************************
 * Function:    DecodeHuffmanQuads
 *
 * Description: decode 4-way vector Huffman codes in the "count1" region of
 *spectrum
 *
 * Inputs:      valid BitStreamInfo struct, pointing to start of quadword codes
 *              pointer to vwxy buffer to received decoded values
 *              maximum number of codewords to decode
 *              index of quadword table (0 = table A, 1 = table B)
 *              number of bits remaining in bitstream
 *
 * Outputs:     quadruples of decoded coefficients in vwxy
 *              updated BitStreamInfo struct
 *
 * Return:      index of the first "zero_part" value (index of the first sample
 *                of the quad word after which all samples are 0)
 *
 * Notes:        si_huff.bit tests every vwxy output in both quad tables
 **************************************************************************************/
// no improvement with section=data
static int DecodeHuffmanQuads(int *vwxy, int nVals, int tabIdx, int bitsLeft, unsigned char *buf, int bitOffset) {
  int i, v, w, x, y;
  int len, maxBits, cachedBits, padBits;
  unsigned int cache;
  unsigned char cw, *tBase;

  if (bitsLeft <= 0)
    return 0;

  tBase = (unsigned char *) quadTable + quadTabOffset[tabIdx];
  maxBits = quadTabMaxBits[tabIdx];

  /* initially fill cache with any partial byte */
  cache = 0;
  cachedBits = (8 - bitOffset) & 0x07;
  if (cachedBits)
    cache = (unsigned int) (*buf++) << (32 - cachedBits);
  bitsLeft -= cachedBits;

  i = padBits = 0;
  while (i < (nVals - 3)) {
    /* refill cache - assumes cachedBits <= 16 */
    if (bitsLeft >= 16) {
      /* load 2 new bytes into left-justified cache */
      cache |= (unsigned int) (*buf++) << (24 - cachedBits);
      cache |= (unsigned int) (*buf++) << (16 - cachedBits);
      cachedBits += 16;
      bitsLeft -= 16;
    } else {
      /* last time through, pad cache with zeros and drain cache */
      if (cachedBits + bitsLeft <= 0)
        return i;
      if (bitsLeft > 0)
        cache |= (unsigned int) (*buf++) << (24 - cachedBits);
      if (bitsLeft > 8)
        cache |= (unsigned int) (*buf++) << (16 - cachedBits);
      cachedBits += bitsLeft;
      bitsLeft = 0;

      cache &= (signed int) 0x80000000 >> (cachedBits - 1);
      padBits = 10;
      cachedBits += padBits; /* okay if this is > 32 (0's automatically shifted
                                in from right) */
    }

    /* largest maxBits = 6, plus 4 for sign bits, so make sure cache has at
     * least 10 bits */
    while (i < (nVals - 3) && cachedBits >= 10) {
      cw = tBase[cache >> (32 - maxBits)];
      len = GetHLenQ(cw);
      cachedBits -= len;
      cache <<= len;

      v = GetCWVQ(cw);
      if (v) {
        ApplySign(v, cache);
        cache <<= 1;
        cachedBits--;
      }
      w = GetCWWQ(cw);
      if (w) {
        ApplySign(w, cache);
        cache <<= 1;
        cachedBits--;
      }
      x = GetCWXQ(cw);
      if (x) {
        ApplySign(x, cache);
        cache <<= 1;
        cachedBits--;
      }
      y = GetCWYQ(cw);
      if (y) {
        ApplySign(y, cache);
        cache <<= 1;
        cachedBits--;
      }

      /* ran out of bits - okay (means we're done) */
      if (cachedBits < padBits)
        return i;

      *vwxy++ = v;
      *vwxy++ = w;
      *vwxy++ = x;
      *vwxy++ = y;
      i += 4;
    }
  }

  /* decoded max number of quad values */
  return i;
}

/**************************************************************************************
 * Function:    DecodeHuffman
 *
 * Description: decode one granule, one channel worth of Huffman codes
 *
 * Inputs:      MP3DecInfo structure filled by UnpackFrameHeader(),
 *UnpackSideInfo(), and UnpackScaleFactors() (for this granule) buffer pointing
 *to start of Huffman data in MP3 frame pointer to bit offset (0-7) indicating
 *starting bit in buf[0] number of bits in the Huffman data section of the frame
 *                (could include padding bits)
 *              index of current granule and channel
 *
 * Outputs:     decoded coefficients in hi->huffDecBuf[ch] (hi pointer in
 *mp3DecInfo) updated bitOffset
 *
 * Return:      length (in bytes) of Huffman codes
 *              bitOffset also returned in parameter (0 = MSB, 7 = LSB of
 *                byte located at buf + offset)
 *              -1 if null input pointers, huffBlockBits < 0, or decoder runs
 *                out of bits prematurely (invalid bitstream)
 **************************************************************************************/
// .data about 1ms faster per frame
int DecodeHuffman(MP3DecInfo *mp3DecInfo, unsigned char *buf, int *bitOffset, int huffBlockBits, int gr, int ch) {
  int r1Start, r2Start, rEnd[4]; /* region boundaries */
  int i, w, bitsUsed, bitsLeft;
  unsigned char *startBuf = buf;

  FrameHeader *fh;
  SideInfo *si;
  SideInfoSub *sis;
  ScaleFactorInfo *sfi;
  HuffmanInfo *hi;

  /* validate pointers */
  if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || !mp3DecInfo->SideInfoPS || !mp3DecInfo->ScaleFactorInfoPS ||
      !mp3DecInfo->HuffmanInfoPS)
    return -1;

  fh = ((FrameHeader *) (mp3DecInfo->FrameHeaderPS));
  si = ((SideInfo *) (mp3DecInfo->SideInfoPS));
  sis = &si->sis[gr][ch];
  sfi = ((ScaleFactorInfo *) (mp3DecInfo->ScaleFactorInfoPS));
  hi = (HuffmanInfo *) (mp3DecInfo->HuffmanInfoPS);

  if (huffBlockBits < 0)
    return -1;

  /* figure out region boundaries (the first 2*bigVals coefficients divided into
   * 3 regions) */
  if (sis->winSwitchFlag && sis->blockType == 2) {
    if (sis->mixedBlock == 0) {
      r1Start = fh->sfBand->s[(sis->region0Count + 1) / 3] * 3;
    } else {
      if (fh->ver == MPEG1) {
        r1Start = fh->sfBand->l[sis->region0Count + 1];
      } else {
        /* see MPEG2 spec for explanation */
        w = fh->sfBand->s[4] - fh->sfBand->s[3];
        r1Start = fh->sfBand->l[6] + 2 * w;
      }
    }
    r2Start = MAX_NSAMP; /* short blocks don't have region 2 */
  } else {
    r1Start = fh->sfBand->l[sis->region0Count + 1];
    r2Start = fh->sfBand->l[sis->region0Count + 1 + sis->region1Count + 1];
  }

  /* offset rEnd index by 1 so first region = rEnd[1] - rEnd[0], etc. */
  rEnd[3] = MIN(MAX_NSAMP, 2 * sis->nBigvals);
  rEnd[2] = MIN(r2Start, rEnd[3]);
  rEnd[1] = MIN(r1Start, rEnd[3]);
  rEnd[0] = 0;

  /* rounds up to first all-zero pair (we don't check last pair for (x,y) ==
   * (non-zero, zero)) */
  hi->nonZeroBound[ch] = rEnd[3];

  /* decode Huffman pairs (rEnd[i] are always even numbers) */
  bitsLeft = huffBlockBits;
  for (i = 0; i < 3; i++) {
    bitsUsed = DecodeHuffmanPairs(hi->huffDecBuf[ch] + rEnd[i], rEnd[i + 1] - rEnd[i], sis->tableSelect[i], bitsLeft,
                                  buf, *bitOffset);
    if (bitsUsed < 0 || bitsUsed > bitsLeft) /* error - overran end of bitstream */
      return -1;

    /* update bitstream position */
    buf += (bitsUsed + *bitOffset) >> 3;
    *bitOffset = (bitsUsed + *bitOffset) & 0x07;
    bitsLeft -= bitsUsed;
  }

  /* decode Huffman quads (if any) */
  hi->nonZeroBound[ch] += DecodeHuffmanQuads(hi->huffDecBuf[ch] + rEnd[3], MAX_NSAMP - rEnd[3], sis->count1TableSelect,
                                             bitsLeft, buf, *bitOffset);

  ASSERT(hi->nonZeroBound[ch] <= MAX_NSAMP);
  for (i = hi->nonZeroBound[ch]; i < MAX_NSAMP; i++)
    hi->huffDecBuf[ch][i] = 0;

  /* If bits used for 576 samples < huffBlockBits, then the extras are
   * considered to be stuffing bits (throw away, but need to return correct
   * bitstream position)
   */
  buf += (bitsLeft + *bitOffset) >> 3;
  *bitOffset = (bitsLeft + *bitOffset) & 0x07;

  return (buf - startBuf);
}

/**************************************************************************************
 * Function:    Dequantize
 *
 * Description: dequantize coefficients, decode stereo, reorder short blocks
 *                (one granule-worth)
 *
 * Inputs:      MP3DecInfo structure filled by UnpackFrameHeader(),
 *UnpackSideInfo(), UnpackScaleFactors(), and DecodeHuffman() (for this granule)
 *              index of current granule
 *
 * Outputs:     dequantized and reordered coefficients in hi->huffDecBuf
 *                (one granule-worth, all channels), format = Q26
 *              operates in-place on huffDecBuf but also needs di->workBuf
 *              updated hi->nonZeroBound index for both channels
 *
 * Return:      0 on success, -1 if null input pointers
 *
 * Notes:       In calling output Q(DQ_FRACBITS_OUT), we assume an implicit bias
 *                of 2^15. Some (floating-point) reference implementations
 *factor this into the 2^(0.25 * gain) scaling explicitly. But to avoid
 *precision loss, we don't do that. Instead take it into account in the final
 *                round to PCM (>> by 15 less than we otherwise would have).
 *              Equivalently, we can think of the dequantized coefficients as
 *                Q(DQ_FRACBITS_OUT - 15) with no implicit bias.
 **************************************************************************************/
int Dequantize(MP3DecInfo *mp3DecInfo, int gr) {
  int i, ch, nSamps, mOut[2];
  FrameHeader *fh;
  SideInfo *si;
  ScaleFactorInfo *sfi;
  HuffmanInfo *hi;
  DequantInfo *di;
  CriticalBandInfo *cbi;

  /* validate pointers */
  if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || !mp3DecInfo->SideInfoPS || !mp3DecInfo->ScaleFactorInfoPS ||
      !mp3DecInfo->HuffmanInfoPS || !mp3DecInfo->DequantInfoPS)
    return -1;

  fh = (FrameHeader *) (mp3DecInfo->FrameHeaderPS);

  /* si is an array of up to 4 structs, stored as gr0ch0, gr0ch1, gr1ch0, gr1ch1
   */
  si = (SideInfo *) (mp3DecInfo->SideInfoPS);
  sfi = (ScaleFactorInfo *) (mp3DecInfo->ScaleFactorInfoPS);
  hi = (HuffmanInfo *) mp3DecInfo->HuffmanInfoPS;
  di = (DequantInfo *) mp3DecInfo->DequantInfoPS;
  cbi = di->cbi;
  mOut[0] = mOut[1] = 0;

  /* dequantize all the samples in each channel */
  for (ch = 0; ch < mp3DecInfo->nChans; ch++) {
    hi->gb[ch] = DequantChannel(hi->huffDecBuf[ch], di->workBuf, &hi->nonZeroBound[ch], fh, &si->sis[gr][ch],
                                &sfi->sfis[gr][ch], &cbi[ch]);
  }

  /* joint stereo processing assumes one guard bit in input samples
   * it's extremely rare not to have at least one gb, so if this is the case
   *   just make a pass over the data and clip to [-2^30+1, 2^30-1]
   * in practice this may never happen
   */
  if (fh->modeExt && (hi->gb[0] < 1 || hi->gb[1] < 1)) {
    for (i = 0; i < hi->nonZeroBound[0]; i++) {
      if (hi->huffDecBuf[0][i] < -0x3fffffff)
        hi->huffDecBuf[0][i] = -0x3fffffff;
      if (hi->huffDecBuf[0][i] > 0x3fffffff)
        hi->huffDecBuf[0][i] = 0x3fffffff;
    }
    for (i = 0; i < hi->nonZeroBound[1]; i++) {
      if (hi->huffDecBuf[1][i] < -0x3fffffff)
        hi->huffDecBuf[1][i] = -0x3fffffff;
      if (hi->huffDecBuf[1][i] > 0x3fffffff)
        hi->huffDecBuf[1][i] = 0x3fffffff;
    }
  }

  /* do mid-side stereo processing, if enabled */
  if (fh->modeExt >> 1) {
    if (fh->modeExt & 0x01) {
      /* intensity stereo enabled - run mid-side up to start of right zero
       * region */
      if (cbi[1].cbType == 0)
        nSamps = fh->sfBand->l[cbi[1].cbEndL + 1];
      else
        nSamps = 3 * fh->sfBand->s[cbi[1].cbEndSMax + 1];
    } else {
      /* intensity stereo disabled - run mid-side on whole spectrum */
      nSamps = MAX(hi->nonZeroBound[0], hi->nonZeroBound[1]);
    }
    MidSideProc(hi->huffDecBuf, nSamps, mOut);
  }

  /* do intensity stereo processing, if enabled */
  if (fh->modeExt & 0x01) {
    nSamps = hi->nonZeroBound[0];
    if (fh->ver == MPEG1) {
      IntensityProcMPEG1(hi->huffDecBuf, nSamps, fh, &sfi->sfis[gr][1], di->cbi, fh->modeExt >> 1,
                         si->sis[gr][1].mixedBlock, mOut);
    } else {
      IntensityProcMPEG2(hi->huffDecBuf, nSamps, fh, &sfi->sfis[gr][1], di->cbi, &sfi->sfjs, fh->modeExt >> 1,
                         si->sis[gr][1].mixedBlock, mOut);
    }
  }

  /* adjust guard bit count and nonZeroBound if we did any stereo processing */
  if (fh->modeExt) {
    hi->gb[0] = CLZ(mOut[0]) - 1;
    hi->gb[1] = CLZ(mOut[1]) - 1;
    nSamps = MAX(hi->nonZeroBound[0], hi->nonZeroBound[1]);
    hi->nonZeroBound[0] = nSamps;
    hi->nonZeroBound[1] = nSamps;
  }

  /* output format Q(DQ_FRACBITS_OUT) */
  return 0;
}

#define COS0_0 0x4013c251  /* Q31 */
#define COS0_1 0x40b345bd  /* Q31 */
#define COS0_2 0x41fa2d6d  /* Q31 */
#define COS0_3 0x43f93421  /* Q31 */
#define COS0_4 0x46cc1bc4  /* Q31 */
#define COS0_5 0x4a9d9cf0  /* Q31 */
#define COS0_6 0x4fae3711  /* Q31 */
#define COS0_7 0x56601ea7  /* Q31 */
#define COS0_8 0x5f4cf6eb  /* Q31 */
#define COS0_9 0x6b6fcf26  /* Q31 */
#define COS0_10 0x7c7d1db3 /* Q31 */
#define COS0_11 0x4ad81a97 /* Q30 */
#define COS0_12 0x5efc8d96 /* Q30 */
#define COS0_13 0x41d95790 /* Q29 */
#define COS0_14 0x6d0b20cf /* Q29 */
#define COS0_15 0x518522fb /* Q27 */

#define COS1_0 0x404f4672 /* Q31 */
#define COS1_1 0x42e13c10 /* Q31 */
#define COS1_2 0x48919f44 /* Q31 */
#define COS1_3 0x52cb0e63 /* Q31 */
#define COS1_4 0x64e2402e /* Q31 */
#define COS1_5 0x43e224a9 /* Q30 */
#define COS1_6 0x6e3c92c1 /* Q30 */
#define COS1_7 0x519e4e04 /* Q28 */

#define COS2_0 0x4140fb46 /* Q31 */
#define COS2_1 0x4cf8de88 /* Q31 */
#define COS2_2 0x73326bbf /* Q31 */
#define COS2_3 0x52036742 /* Q29 */

#define COS3_0 0x4545e9ef /* Q31 */
#define COS3_1 0x539eba45 /* Q30 */

#define COS4_0 0x5a82799a /* Q31 */

// faster in ROM
static const int dcttab[48] = {
    /* first pass */
    COS0_0, COS0_15, COS1_0,  /* 31, 27, 31 */
    COS0_1, COS0_14, COS1_1,  /* 31, 29, 31 */
    COS0_2, COS0_13, COS1_2,  /* 31, 29, 31 */
    COS0_3, COS0_12, COS1_3,  /* 31, 30, 31 */
    COS0_4, COS0_11, COS1_4,  /* 31, 30, 31 */
    COS0_5, COS0_10, COS1_5,  /* 31, 31, 30 */
    COS0_6, COS0_9, COS1_6,   /* 31, 31, 30 */
    COS0_7, COS0_8, COS1_7,   /* 31, 31, 28 */
                              /* second pass */
    COS2_0, COS2_3, COS3_0,   /* 31, 29, 31 */
    COS2_1, COS2_2, COS3_1,   /* 31, 31, 30 */
    -COS2_0, -COS2_3, COS3_0, /* 31, 29, 31 */
    -COS2_1, -COS2_2, COS3_1, /* 31, 31, 30 */
    COS2_0, COS2_3, COS3_0,   /* 31, 29, 31 */
    COS2_1, COS2_2, COS3_1,   /* 31, 31, 30 */
    -COS2_0, -COS2_3, COS3_0, /* 31, 29, 31 */
    -COS2_1, -COS2_2, COS3_1, /* 31, 31, 30 */
};

#define D32FP(i, s0, s1, s2) \
  { \
    a0 = buf[i]; \
    a3 = buf[31 - i]; \
    a1 = buf[15 - i]; \
    a2 = buf[16 + i]; \
    b0 = a0 + a3; \
    b3 = MULSHIFT32(*cptr++, a0 - a3) << (s0); \
    b1 = a1 + a2; \
    b2 = MULSHIFT32(*cptr++, a1 - a2) << (s1); \
    buf[i] = b0 + b1; \
    buf[15 - i] = MULSHIFT32(*cptr, b0 - b1) << (s2); \
    buf[16 + i] = b2 + b3; \
    buf[31 - i] = MULSHIFT32(*cptr++, b3 - b2) << (s2); \
  }

/**************************************************************************************
 * Function:    FDCT32
 *
 * Description: Ken's highly-optimized 32-point DCT (radix-4 + radix-8)
 *
 * Inputs:      input buffer, length = 32 samples
 *              require at least 6 guard bits in input vector x to avoid
 *possibility of overflow in internal calculations (see bbtest_imdct test app)
 *              buffer offset and oddblock flag for polyphase filter input
 *buffer number of guard bits in input
 *
 * Outputs:     output buffer, data copied and interleaved for polyphase filter
 *              no guarantees about number of guard bits in output
 *
 * Return:      none
 *
 * Notes:       number of muls = 4*8 + 12*4 = 80
 *              final stage of DCT is hardcoded to shuffle data into the proper
 *order for the polyphase filterbank fully unrolled stage 1, for max precision
 *(scale the 1/cos() factors differently, depending on magnitude) guard bit
 *analysis verified by exhaustive testing of all 2^32 combinations of max
 *pos/max neg values in x[]
 *
 * TODO:        code organization and optimization for ARM
 *              possibly interleave stereo (cut # of coef loads in half - may
 *not have enough registers)
 **************************************************************************************/
// about 1ms faster in RAM
void FDCT32(int *buf, int *dest, int offset, int oddBlock, int gb) {
  int i, s, tmp, es;
  const int *cptr = dcttab;
  int a0, a1, a2, a3, a4, a5, a6, a7;
  int b0, b1, b2, b3, b4, b5, b6, b7;
  int *d;

  /* scaling - ensure at least 6 guard bits for DCT
   * (in practice this is already true 99% of time, so this code is
   *  almost never triggered)
   */
  es = 0;
  if (gb < 6) {
    es = 6 - gb;
    for (i = 0; i < 32; i++)
      buf[i] >>= es;
  }

  /* first pass */
  D32FP(0, 1, 5, 1);
  D32FP(1, 1, 3, 1);
  D32FP(2, 1, 3, 1);
  D32FP(3, 1, 2, 1);
  D32FP(4, 1, 2, 1);
  D32FP(5, 1, 1, 2);
  D32FP(6, 1, 1, 2);
  D32FP(7, 1, 1, 4);

  /* second pass */
  for (i = 4; i > 0; i--) {
    a0 = buf[0];
    a7 = buf[7];
    a3 = buf[3];
    a4 = buf[4];
    b0 = a0 + a7;
    b7 = MULSHIFT32(*cptr++, a0 - a7) << 1;
    b3 = a3 + a4;
    b4 = MULSHIFT32(*cptr++, a3 - a4) << 3;
    a0 = b0 + b3;
    a3 = MULSHIFT32(*cptr, b0 - b3) << 1;
    a4 = b4 + b7;
    a7 = MULSHIFT32(*cptr++, b7 - b4) << 1;

    a1 = buf[1];
    a6 = buf[6];
    a2 = buf[2];
    a5 = buf[5];
    b1 = a1 + a6;
    b6 = MULSHIFT32(*cptr++, a1 - a6) << 1;
    b2 = a2 + a5;
    b5 = MULSHIFT32(*cptr++, a2 - a5) << 1;
    a1 = b1 + b2;
    a2 = MULSHIFT32(*cptr, b1 - b2) << 2;
    a5 = b5 + b6;
    a6 = MULSHIFT32(*cptr++, b6 - b5) << 2;

    b0 = a0 + a1;
    b1 = MULSHIFT32(COS4_0, a0 - a1) << 1;
    b2 = a2 + a3;
    b3 = MULSHIFT32(COS4_0, a3 - a2) << 1;
    buf[0] = b0;
    buf[1] = b1;
    buf[2] = b2 + b3;
    buf[3] = b3;

    b4 = a4 + a5;
    b5 = MULSHIFT32(COS4_0, a4 - a5) << 1;
    b6 = a6 + a7;
    b7 = MULSHIFT32(COS4_0, a7 - a6) << 1;
    b6 += b7;
    buf[4] = b4 + b6;
    buf[5] = b5 + b7;
    buf[6] = b5 + b6;
    buf[7] = b7;

    buf += 8;
  }
  buf -= 32; /* reset */

  /* sample 0 - always delayed one block */
  d = dest + 64 * 16 + ((offset - oddBlock) & 7) + (oddBlock ? 0 : VBUF_LENGTH);
  s = buf[0];
  d[0] = d[8] = s;

  /* samples 16 to 31 */
  d = dest + offset + (oddBlock ? VBUF_LENGTH : 0);

  s = buf[1];
  d[0] = d[8] = s;
  d += 64;

  tmp = buf[25] + buf[29];
  s = buf[17] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[9] + buf[13];
  d[0] = d[8] = s;
  d += 64;
  s = buf[21] + tmp;
  d[0] = d[8] = s;
  d += 64;

  tmp = buf[29] + buf[27];
  s = buf[5];
  d[0] = d[8] = s;
  d += 64;
  s = buf[21] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[13] + buf[11];
  d[0] = d[8] = s;
  d += 64;
  s = buf[19] + tmp;
  d[0] = d[8] = s;
  d += 64;

  tmp = buf[27] + buf[31];
  s = buf[3];
  d[0] = d[8] = s;
  d += 64;
  s = buf[19] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[11] + buf[15];
  d[0] = d[8] = s;
  d += 64;
  s = buf[23] + tmp;
  d[0] = d[8] = s;
  d += 64;

  tmp = buf[31];
  s = buf[7];
  d[0] = d[8] = s;
  d += 64;
  s = buf[23] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[15];
  d[0] = d[8] = s;
  d += 64;
  s = tmp;
  d[0] = d[8] = s;

  /* samples 16 to 1 (sample 16 used again) */
  d = dest + 16 + ((offset - oddBlock) & 7) + (oddBlock ? 0 : VBUF_LENGTH);

  s = buf[1];
  d[0] = d[8] = s;
  d += 64;

  tmp = buf[30] + buf[25];
  s = buf[17] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[14] + buf[9];
  d[0] = d[8] = s;
  d += 64;
  s = buf[22] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[6];
  d[0] = d[8] = s;
  d += 64;

  tmp = buf[26] + buf[30];
  s = buf[22] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[10] + buf[14];
  d[0] = d[8] = s;
  d += 64;
  s = buf[18] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[2];
  d[0] = d[8] = s;
  d += 64;

  tmp = buf[28] + buf[26];
  s = buf[18] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[12] + buf[10];
  d[0] = d[8] = s;
  d += 64;
  s = buf[20] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[4];
  d[0] = d[8] = s;
  d += 64;

  tmp = buf[24] + buf[28];
  s = buf[20] + tmp;
  d[0] = d[8] = s;
  d += 64;
  s = buf[8] + buf[12];
  d[0] = d[8] = s;
  d += 64;
  s = buf[16] + tmp;
  d[0] = d[8] = s;

  /* this is so rarely invoked that it's not worth making two versions of the
   * output shuffle code (one for no shift, one for clip + variable shift) like
   * in IMDCT here we just load, clip, shift, and store on the rare instances
   * that es != 0
   */
  if (es) {
    d = dest + 64 * 16 + ((offset - oddBlock) & 7) + (oddBlock ? 0 : VBUF_LENGTH);
    s = d[0];
    CLIP_2N(s, 31 - es);
    d[0] = d[8] = (s << es);

    d = dest + offset + (oddBlock ? VBUF_LENGTH : 0);
    for (i = 16; i <= 31; i++) {
      s = d[0];
      CLIP_2N(s, 31 - es);
      d[0] = d[8] = (s << es);
      d += 64;
    }

    d = dest + 16 + ((offset - oddBlock) & 7) + (oddBlock ? 0 : VBUF_LENGTH);
    for (i = 15; i >= 0; i--) {
      s = d[0];
      CLIP_2N(s, 31 - es);
      d[0] = d[8] = (s << es);
      d += 64;
    }
  }
}

/**************************************************************************************
 * Function:    ClearBuffer
 *
 * Description: fill buffer with 0's
 *
 * Inputs:      pointer to buffer
 *              number of bytes to fill with 0
 *
 * Outputs:     cleared buffer
 *
 * Return:      none
 *
 * Notes:       slow, platform-independent equivalent to memset(buf, 0, nBytes)
 **************************************************************************************/
static void ClearBuffer(void *buf, int nBytes) {
  int i;
  unsigned char *cbuf = (unsigned char *) buf;

  for (i = 0; i < nBytes; i++)
    cbuf[i] = 0;

  return;
}

/**************************************************************************************
 * Function:    AllocateBuffers
 *
 * Description: allocate all the memory needed for the MP3 decoder
 *
 * Inputs:      none
 *
 * Outputs:     none
 *
 * Return:      pointer to MP3DecInfo structure (initialized with pointers to
 *all the internal buffers needed for decoding, all other members of MP3DecInfo
 *structure set to 0)
 *
 * Notes:       if one or more mallocs fail, function frees any buffers already
 *                allocated before returning
 **************************************************************************************/
MP3DecInfo *AllocateBuffers(void) {
  MP3DecInfo *mp3DecInfo;
  FrameHeader *fh;
  SideInfo *si;
  ScaleFactorInfo *sfi;
  HuffmanInfo *hi;
  DequantInfo *di;
  IMDCTInfo *mi;
  SubbandInfo *sbi;

  esphome::ExternalRAMAllocator<MP3DecInfo> mp3di_allocator(esphome::ExternalRAMAllocator<MP3DecInfo>::ALLOW_FAILURE);
  esphome::ExternalRAMAllocator<FrameHeader> fh_allocator(esphome::ExternalRAMAllocator<FrameHeader>::ALLOW_FAILURE);
  esphome::ExternalRAMAllocator<SideInfo> si_allocator(esphome::ExternalRAMAllocator<SideInfo>::ALLOW_FAILURE);
  esphome::ExternalRAMAllocator<ScaleFactorInfo> sfi_allocator(
      esphome::ExternalRAMAllocator<ScaleFactorInfo>::ALLOW_FAILURE);
  esphome::ExternalRAMAllocator<HuffmanInfo> hi_allocator(esphome::ExternalRAMAllocator<HuffmanInfo>::ALLOW_FAILURE);
  esphome::ExternalRAMAllocator<DequantInfo> di_allocator(esphome::ExternalRAMAllocator<DequantInfo>::ALLOW_FAILURE);
  esphome::ExternalRAMAllocator<IMDCTInfo> mi_allocator(esphome::ExternalRAMAllocator<IMDCTInfo>::ALLOW_FAILURE);
  esphome::ExternalRAMAllocator<SubbandInfo> sbi_allocator(esphome::ExternalRAMAllocator<SubbandInfo>::ALLOW_FAILURE);

  // mp3DecInfo = (MP3DecInfo *)malloc(sizeof(MP3DecInfo));
  mp3DecInfo = mp3di_allocator.allocate(1);
  if (!mp3DecInfo)
    return 0;
  ClearBuffer(mp3DecInfo, sizeof(MP3DecInfo));

  // fh = (FrameHeader *)malloc(sizeof(FrameHeader));
  // si = (SideInfo *)malloc(sizeof(SideInfo));
  // sfi = (ScaleFactorInfo *)malloc(sizeof(ScaleFactorInfo));
  // hi = (HuffmanInfo *)malloc(sizeof(HuffmanInfo));
  // di = (DequantInfo *)malloc(sizeof(DequantInfo));
  // mi = (IMDCTInfo *)malloc(sizeof(IMDCTInfo));
  // sbi = (SubbandInfo *)malloc(sizeof(SubbandInfo));
  fh = fh_allocator.allocate(1);
  si = si_allocator.allocate(1);
  sfi = sfi_allocator.allocate(1);
  hi = hi_allocator.allocate(1);
  di = di_allocator.allocate(1);
  mi = mi_allocator.allocate(1);
  sbi = sbi_allocator.allocate(1);

  mp3DecInfo->FrameHeaderPS = (void *) fh;
  mp3DecInfo->SideInfoPS = (void *) si;
  mp3DecInfo->ScaleFactorInfoPS = (void *) sfi;
  mp3DecInfo->HuffmanInfoPS = (void *) hi;
  mp3DecInfo->DequantInfoPS = (void *) di;
  mp3DecInfo->IMDCTInfoPS = (void *) mi;
  mp3DecInfo->SubbandInfoPS = (void *) sbi;

  if (!fh || !si || !sfi || !hi || !di || !mi || !sbi) {
    FreeBuffers(mp3DecInfo); /* safe to call - only frees memory that was
                                successfully allocated */
    return 0;
  }

  /* important to do this - DSP primitives assume a bunch of state variables are
   * 0 on first use */
  ClearBuffer(fh, sizeof(FrameHeader));
  ClearBuffer(si, sizeof(SideInfo));
  ClearBuffer(sfi, sizeof(ScaleFactorInfo));
  ClearBuffer(hi, sizeof(HuffmanInfo));
  ClearBuffer(di, sizeof(DequantInfo));
  ClearBuffer(mi, sizeof(IMDCTInfo));
  ClearBuffer(sbi, sizeof(SubbandInfo));

  return mp3DecInfo;
}

#define SAFE_FREE(x) \
  { \
    if (x) \
      free(x); \
    (x) = 0; \
  } /* helper macro */

/**************************************************************************************
 * Function:    FreeBuffers
 *
 * Description: frees all the memory used by the MP3 decoder
 *
 * Inputs:      pointer to initialized MP3DecInfo structure
 *
 * Outputs:     none
 *
 * Return:      none
 *
 * Notes:       safe to call even if some buffers were not allocated (uses
 *SAFE_FREE)
 **************************************************************************************/
void FreeBuffers(MP3DecInfo *mp3DecInfo) {
  if (!mp3DecInfo)
    return;

  SAFE_FREE(mp3DecInfo->FrameHeaderPS);
  SAFE_FREE(mp3DecInfo->SideInfoPS);
  SAFE_FREE(mp3DecInfo->ScaleFactorInfoPS);
  SAFE_FREE(mp3DecInfo->HuffmanInfoPS);
  SAFE_FREE(mp3DecInfo->DequantInfoPS);
  SAFE_FREE(mp3DecInfo->IMDCTInfoPS);
  SAFE_FREE(mp3DecInfo->SubbandInfoPS);

  SAFE_FREE(mp3DecInfo);
}

/**************************************************************************************
 * Function:    SetBitstreamPointer
 *
 * Description: initialize bitstream reader
 *
 * Inputs:      pointer to BitStreamInfo struct
 *              number of bytes in bitstream
 *              pointer to byte-aligned buffer of data to read from
 *
 * Outputs:     filled bitstream info struct
 *
 * Return:      none
 **************************************************************************************/
void SetBitstreamPointer(BitStreamInfo *bsi, int nBytes, unsigned char *buf) {
  /* init bitstream */
  bsi->bytePtr = buf;
  bsi->iCache = 0;     /* 4-byte unsigned int */
  bsi->cachedBits = 0; /* i.e. zero bits in cache */
  bsi->nBytes = nBytes;
}

/**************************************************************************************
 * Function:    RefillBitstreamCache
 *
 * Description: read new data from bitstream buffer into bsi cache
 *
 * Inputs:      pointer to initialized BitStreamInfo struct
 *
 * Outputs:     updated bitstream info struct
 *
 * Return:      none
 *
 * Notes:       only call when iCache is completely drained (resets bitOffset to
 *0) always loads 4 new bytes except when bsi->nBytes < 4 (end of buffer) stores
 *data as big-endian in cache, regardless of machine endian-ness
 *
 * TODO:        optimize for ARM
 *              possibly add little/big-endian modes for doing 32-bit loads
 **************************************************************************************/
static __inline void RefillBitstreamCache(BitStreamInfo *bsi) {
  int nBytes = bsi->nBytes;

  /* optimize for common case, independent of machine endian-ness */
  if (nBytes >= 4) {
    bsi->iCache = (*bsi->bytePtr++) << 24;
    bsi->iCache |= (*bsi->bytePtr++) << 16;
    bsi->iCache |= (*bsi->bytePtr++) << 8;
    bsi->iCache |= (*bsi->bytePtr++);
    bsi->cachedBits = 32;
    bsi->nBytes -= 4;
  } else {
    bsi->iCache = 0;
    while (nBytes--) {
      bsi->iCache |= (*bsi->bytePtr++);
      bsi->iCache <<= 8;
    }
    bsi->iCache <<= ((3 - bsi->nBytes) * 8);
    bsi->cachedBits = 8 * bsi->nBytes;
    bsi->nBytes = 0;
  }
}

/**************************************************************************************
 * Function:    GetBits
 *
 * Description: get bits from bitstream, advance bitstream pointer
 *
 * Inputs:      pointer to initialized BitStreamInfo struct
 *              number of bits to get from bitstream
 *
 * Outputs:     updated bitstream info struct
 *
 * Return:      the next nBits bits of data from bitstream buffer
 *
 * Notes:       nBits must be in range [0, 31], nBits outside this range masked
 *by 0x1f for speed, does not indicate error if you overrun bit buffer if nBits
 *= 0, returns 0 (useful for scalefactor unpacking)
 *
 * TODO:        optimize for ARM
 **************************************************************************************/
unsigned int GetBits(BitStreamInfo *bsi, int nBits) {
  unsigned int data, lowBits;

  nBits &= 0x1f;                      /* nBits mod 32 to avoid unpredictable results like >> by
                                         negative amount */
  data = bsi->iCache >> (31 - nBits); /* unsigned >> so zero-extend */
  data >>= 1;                         /* do as >> 31, >> 1 so that nBits = 0 works okay (returns 0) */
  bsi->iCache <<= nBits;              /* left-justify cache */
  bsi->cachedBits -= nBits;           /* how many bits have we drawn from the cache so far */

  /* if we cross an int boundary, refill the cache */
  if (bsi->cachedBits < 0) {
    lowBits = -bsi->cachedBits;
    RefillBitstreamCache(bsi);
    data |= bsi->iCache >> (32 - lowBits); /* get the low-order bits */

    bsi->cachedBits -= lowBits; /* how many bits have we drawn from the cache so far */
    bsi->iCache <<= lowBits;    /* left-justify cache */
  }

  return data;
}

/**************************************************************************************
 * Function:    CalcBitsUsed
 *
 * Description: calculate how many bits have been read from bitstream
 *
 * Inputs:      pointer to initialized BitStreamInfo struct
 *              pointer to start of bitstream buffer
 *              bit offset into first byte of startBuf (0-7)
 *
 * Outputs:     none
 *
 * Return:      number of bits read from bitstream, as offset from
 *startBuf:startOffset
 **************************************************************************************/
int CalcBitsUsed(BitStreamInfo *bsi, unsigned char *startBuf, int startOffset) {
  int bitsUsed;

  bitsUsed = (bsi->bytePtr - startBuf) * 8;
  bitsUsed -= bsi->cachedBits;
  bitsUsed -= startOffset;

  return bitsUsed;
}

/**************************************************************************************
 * Function:    CheckPadBit
 *
 * Description: check whether padding byte is present in an MP3 frame
 *
 * Inputs:      MP3DecInfo struct with valid FrameHeader struct
 *                (filled by UnpackFrameHeader())
 *
 * Outputs:     none
 *
 * Return:      1 if pad bit is set, 0 if not, -1 if null input pointer
 **************************************************************************************/
int CheckPadBit(MP3DecInfo *mp3DecInfo) {
  FrameHeader *fh;

  /* validate pointers */
  if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS)
    return -1;

  fh = ((FrameHeader *) (mp3DecInfo->FrameHeaderPS));

  return (fh->paddingBit ? 1 : 0);
}

/**************************************************************************************
 * Function:    UnpackFrameHeader
 *
 * Description: parse the fields of the MP3 frame header
 *
 * Inputs:      buffer pointing to a complete MP3 frame header (4 bytes, plus 2
 *if CRC)
 *
 * Outputs:     filled frame header info in the MP3DecInfo structure
 *              updated platform-specific FrameHeader struct
 *
 * Return:      length (in bytes) of frame header (for caller to calculate
 *offset to first byte following frame header) -1 if null frameHeader or invalid
 *header
 *
 * TODO:        check for valid modes, depending on capabilities of decoder
 *              test CRC on actual stream (verify no endian problems)
 **************************************************************************************/
int UnpackFrameHeader(MP3DecInfo *mp3DecInfo, unsigned char *buf) {
  int verIdx;
  FrameHeader *fh;

  /* validate pointers and sync word */
  if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || (buf[0] & SYNCWORDH) != SYNCWORDH ||
      (buf[1] & SYNCWORDL) != SYNCWORDL)
    return -1;

  fh = ((FrameHeader *) (mp3DecInfo->FrameHeaderPS));

  /* read header fields - use bitmasks instead of GetBits() for speed, since
   * format never varies */
  verIdx = (buf[1] >> 3) & 0x03;
  fh->ver = (MPEGVersion) (verIdx == 0 ? MPEG25 : ((verIdx & 0x01) ? MPEG1 : MPEG2));
  fh->layer = 4 - ((buf[1] >> 1) & 0x03); /* easy mapping of index to layer number, 4 = error */
  fh->crc = 1 - ((buf[1] >> 0) & 0x01);
  fh->brIdx = (buf[2] >> 4) & 0x0f;
  fh->srIdx = (buf[2] >> 2) & 0x03;
  fh->paddingBit = (buf[2] >> 1) & 0x01;
  fh->privateBit = (buf[2] >> 0) & 0x01;
  fh->sMode = (StereoMode) ((buf[3] >> 6) & 0x03); /* maps to correct enum (see definition) */
  fh->modeExt = (buf[3] >> 4) & 0x03;
  fh->copyFlag = (buf[3] >> 3) & 0x01;
  fh->origFlag = (buf[3] >> 2) & 0x01;
  fh->emphasis = (buf[3] >> 0) & 0x03;

  /* check parameters to avoid indexing tables with bad values */
  if (fh->srIdx == 3 || fh->layer == 4 || fh->brIdx == 15)
    return -1;

  fh->sfBand = &sfBandTable[fh->ver][fh->srIdx]; /* for readability (we reference
                                                    sfBandTable many times in decoder) */
  if (fh->sMode != Joint)                        /* just to be safe (dequant, stproc check fh->modeExt) */
    fh->modeExt = 0;

  /* init user-accessible data */
  mp3DecInfo->nChans = (fh->sMode == Mono ? 1 : 2);
  mp3DecInfo->samprate = samplerateTab[fh->ver][fh->srIdx];
  mp3DecInfo->nGrans = (fh->ver == MPEG1 ? NGRANS_MPEG1 : NGRANS_MPEG2);
  mp3DecInfo->nGranSamps = ((int) samplesPerFrameTab[fh->ver][fh->layer - 1]) / mp3DecInfo->nGrans;
  mp3DecInfo->layer = fh->layer;
  mp3DecInfo->version = fh->ver;

  /* get bitrate and nSlots from table, unless brIdx == 0 (free mode) in which
   * case caller must figure it out himself question - do we want to overwrite
   * mp3DecInfo->bitrate with 0 each time if it's free mode, and copy the
   * pre-calculated actual free bitrate into it in mp3dec.c (according to the
   * spec, this shouldn't be necessary, since it should be either all frames
   * free or none free)
   */
  if (fh->brIdx) {
    mp3DecInfo->bitrate = ((int) bitrateTab[fh->ver][fh->layer - 1][fh->brIdx]) * 1000;

    /* nSlots = total frame bytes (from table) - sideInfo bytes - header - CRC
     * (if present) + pad (if present) */
    mp3DecInfo->nSlots = (int) slotTab[fh->ver][fh->srIdx][fh->brIdx] -
                         (int) sideBytesTab[fh->ver][(fh->sMode == Mono ? 0 : 1)] - 4 - (fh->crc ? 2 : 0) +
                         (fh->paddingBit ? 1 : 0);
  }

  /* load crc word, if enabled, and return length of frame header (in bytes) */
  if (fh->crc) {
    fh->CRCWord = ((int) buf[4] << 8 | (int) buf[5] << 0);
    return 6;
  } else {
    fh->CRCWord = 0;
    return 4;
  }
}

/**************************************************************************************
 * Function:    UnpackSideInfo
 *
 * Description: parse the fields of the MP3 side info header
 *
 * Inputs:      MP3DecInfo structure filled by UnpackFrameHeader()
 *              buffer pointing to the MP3 side info data
 *
 * Outputs:     updated mainDataBegin in MP3DecInfo struct
 *              updated private (platform-specific) SideInfo struct
 *
 * Return:      length (in bytes) of side info data
 *              -1 if null input pointers
 **************************************************************************************/
int UnpackSideInfo(MP3DecInfo *mp3DecInfo, unsigned char *buf) {
  int gr, ch, bd, nBytes;
  BitStreamInfo bitStreamInfo, *bsi;
  FrameHeader *fh;
  SideInfo *si;
  SideInfoSub *sis;

  /* validate pointers and sync word */
  if (!mp3DecInfo || !mp3DecInfo->FrameHeaderPS || !mp3DecInfo->SideInfoPS)
    return -1;

  fh = ((FrameHeader *) (mp3DecInfo->FrameHeaderPS));
  si = ((SideInfo *) (mp3DecInfo->SideInfoPS));

  bsi = &bitStreamInfo;
  if (fh->ver == MPEG1) {
    /* MPEG 1 */
    nBytes = (fh->sMode == Mono ? SIBYTES_MPEG1_MONO : SIBYTES_MPEG1_STEREO);
    SetBitstreamPointer(bsi, nBytes, buf);
    si->mainDataBegin = GetBits(bsi, 9);
    si->privateBits = GetBits(bsi, (fh->sMode == Mono ? 5 : 3));

    for (ch = 0; ch < mp3DecInfo->nChans; ch++)
      for (bd = 0; bd < MAX_SCFBD; bd++)
        si->scfsi[ch][bd] = GetBits(bsi, 1);
  } else {
    /* MPEG 2, MPEG 2.5 */
    nBytes = (fh->sMode == Mono ? SIBYTES_MPEG2_MONO : SIBYTES_MPEG2_STEREO);
    SetBitstreamPointer(bsi, nBytes, buf);
    si->mainDataBegin = GetBits(bsi, 8);
    si->privateBits = GetBits(bsi, (fh->sMode == Mono ? 1 : 2));
  }

  for (gr = 0; gr < mp3DecInfo->nGrans; gr++) {
    for (ch = 0; ch < mp3DecInfo->nChans; ch++) {
      sis = &si->sis[gr][ch]; /* side info subblock for this granule, channel */

      sis->part23Length = GetBits(bsi, 12);
      sis->nBigvals = GetBits(bsi, 9);
      sis->globalGain = GetBits(bsi, 8);
      sis->sfCompress = GetBits(bsi, (fh->ver == MPEG1 ? 4 : 9));
      sis->winSwitchFlag = GetBits(bsi, 1);

      if (sis->winSwitchFlag) {
        /* this is a start, stop, short, or mixed block */
        sis->blockType = GetBits(bsi, 2);  /* 0 = normal, 1 = start, 2 = short, 3 = stop */
        sis->mixedBlock = GetBits(bsi, 1); /* 0 = not mixed, 1 = mixed */
        sis->tableSelect[0] = GetBits(bsi, 5);
        sis->tableSelect[1] = GetBits(bsi, 5);
        sis->tableSelect[2] = 0; /* unused */
        sis->subBlockGain[0] = GetBits(bsi, 3);
        sis->subBlockGain[1] = GetBits(bsi, 3);
        sis->subBlockGain[2] = GetBits(bsi, 3);

        /* TODO - check logic */
        if (sis->blockType == 0) {
          /* this should not be allowed, according to spec */
          sis->nBigvals = 0;
          sis->part23Length = 0;
          sis->sfCompress = 0;
        } else if (sis->blockType == 2 && sis->mixedBlock == 0) {
          /* short block, not mixed */
          sis->region0Count = 8;
        } else {
          /* start, stop, or short-mixed */
          sis->region0Count = 7;
        }
        sis->region1Count = 20 - sis->region0Count;
      } else {
        /* this is a normal block */
        sis->blockType = 0;
        sis->mixedBlock = 0;
        sis->tableSelect[0] = GetBits(bsi, 5);
        sis->tableSelect[1] = GetBits(bsi, 5);
        sis->tableSelect[2] = GetBits(bsi, 5);
        sis->region0Count = GetBits(bsi, 4);
        sis->region1Count = GetBits(bsi, 3);
      }
      sis->preFlag = (fh->ver == MPEG1 ? GetBits(bsi, 1) : 0);
      sis->sfactScale = GetBits(bsi, 1);
      sis->count1TableSelect = GetBits(bsi, 1);
    }
  }
  mp3DecInfo->mainDataBegin = si->mainDataBegin; /* needed by main decode loop */

  ASSERT(nBytes == CalcBitsUsed(bsi, buf, 0) >> 3);

  return nBytes;
}

/**************************************************************************************
 * Function:    MP3InitDecoder
 *
 * Description: allocate memory for platform-specific data
 *              clear all the user-accessible fields
 *
 * Inputs:      none
 *
 * Outputs:     none
 *
 * Return:      handle to mp3 decoder instance, 0 if malloc fails
 **************************************************************************************/
HMP3Decoder MP3InitDecoder(void) {
  MP3DecInfo *mp3DecInfo;

  mp3DecInfo = AllocateBuffers();

  return (HMP3Decoder) mp3DecInfo;
}

/**************************************************************************************
 * Function:    MP3FreeDecoder
 *
 * Description: free platform-specific data allocated by InitMP3Decoder
 *              zero out the contents of MP3DecInfo struct
 *
 * Inputs:      valid MP3 decoder instance pointer (HMP3Decoder)
 *
 * Outputs:     none
 *
 * Return:      none
 **************************************************************************************/
void MP3FreeDecoder(HMP3Decoder hMP3Decoder) {
  MP3DecInfo *mp3DecInfo = (MP3DecInfo *) hMP3Decoder;

  if (!mp3DecInfo)
    return;

  FreeBuffers(mp3DecInfo);
}

/**************************************************************************************
 * Function:    MP3FindSyncWord
 *
 * Description: locate the next byte-alinged sync word in the raw mp3 stream
 *
 * Inputs:      buffer to search for sync word
 *              max number of bytes to search in buffer
 *
 * Outputs:     none
 *
 * Return:      offset to first sync word (bytes from start of buf)
 *              -1 if sync not found after searching nBytes
 **************************************************************************************/
int MP3FindSyncWord(unsigned char *buf, int nBytes) {
  int i;

  /* find byte-aligned syncword - need 12 (MPEG 1,2) or 11 (MPEG 2.5) matching
   * bits */
  for (i = 0; i < nBytes - 1; i++) {
    if ((buf[i + 0] & SYNCWORDH) == SYNCWORDH && (buf[i + 1] & SYNCWORDL) == SYNCWORDL)
      return i;
  }

  return -1;
}

/**************************************************************************************
 * Function:    MP3FindFreeSync
 *
 * Description: figure out number of bytes between adjacent sync words in "free"
 *mode
 *
 * Inputs:      buffer to search for next sync word
 *              the 4-byte frame header starting at the current sync word
 *              max number of bytes to search in buffer
 *
 * Outputs:     none
 *
 * Return:      offset to next sync word, minus any pad byte (i.e. nSlots)
 *              -1 if sync not found after searching nBytes
 *
 * Notes:       this checks that the first 22 bits of the next frame header are
 *the same as the current frame header, but it's still not foolproof (could
 *accidentally find a sequence in the bitstream which appears to match but is
 *not actually the next frame header) this could be made more error-resilient by
 *checking several frames in a row and verifying that nSlots is the same in each
 *case since free mode requires CBR (see spec) we generally only call this
 *function once (first frame) then store the result (nSlots) and just use it
 *from then on
 **************************************************************************************/
static int MP3FindFreeSync(unsigned char *buf, unsigned char firstFH[4], int nBytes) {
  int offset = 0;
  unsigned char *bufPtr = buf;

  /* loop until we either:
   *  - run out of nBytes (FindMP3SyncWord() returns -1)
   *  - find the next valid frame header (sync word, version, layer, CRC flag,
   * bitrate, and sample rate in next header must match current header)
   */
  while (1) {
    offset = MP3FindSyncWord(bufPtr, nBytes);
    bufPtr += offset;
    if (offset < 0) {
      return -1;
    } else if ((bufPtr[0] == firstFH[0]) && (bufPtr[1] == firstFH[1]) && ((bufPtr[2] & 0xfc) == (firstFH[2] & 0xfc))) {
      /* want to return number of bytes per frame, NOT counting the padding
       * byte, so subtract one if padFlag == 1 */
      if ((firstFH[2] >> 1) & 0x01)
        bufPtr--;
      return bufPtr - buf;
    }
    bufPtr += 3;
    nBytes -= (offset + 3);
  };

  return -1;
}

/**************************************************************************************
 * Function:    MP3GetLastFrameInfo
 *
 * Description: get info about last MP3 frame decoded (number of sampled
 *decoded, sample rate, bitrate, etc.)
 *
 * Inputs:      valid MP3 decoder instance pointer (HMP3Decoder)
 *              pointer to MP3FrameInfo struct
 *
 * Outputs:     filled-in MP3FrameInfo struct
 *
 * Return:      none
 *
 * Notes:       call this right after calling MP3Decode
 **************************************************************************************/
void MP3GetLastFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo) {
  MP3DecInfo *mp3DecInfo = (MP3DecInfo *) hMP3Decoder;

  if (!mp3DecInfo || mp3DecInfo->layer != 3) {
    mp3FrameInfo->bitrate = 0;
    mp3FrameInfo->nChans = 0;
    mp3FrameInfo->samprate = 0;
    mp3FrameInfo->bitsPerSample = 0;
    mp3FrameInfo->outputSamps = 0;
    mp3FrameInfo->layer = 0;
    mp3FrameInfo->version = 0;
  } else {
    mp3FrameInfo->bitrate = mp3DecInfo->bitrate;
    mp3FrameInfo->nChans = mp3DecInfo->nChans;
    mp3FrameInfo->samprate = mp3DecInfo->samprate;
    mp3FrameInfo->bitsPerSample = 16;
    mp3FrameInfo->outputSamps =
        mp3DecInfo->nChans * (int) samplesPerFrameTab[mp3DecInfo->version][mp3DecInfo->layer - 1];
    mp3FrameInfo->layer = mp3DecInfo->layer;
    mp3FrameInfo->version = mp3DecInfo->version;
  }
}

/**************************************************************************************
 * Function:    MP3GetNextFrameInfo
 *
 * Description: parse MP3 frame header
 *
 * Inputs:      valid MP3 decoder instance pointer (HMP3Decoder)
 *              pointer to MP3FrameInfo struct
 *              pointer to buffer containing valid MP3 frame header (located
 *using MP3FindSyncWord(), above)
 *
 * Outputs:     filled-in MP3FrameInfo struct
 *
 * Return:      error code, defined in mp3dec.h (0 means no error, < 0 means
 *error)
 **************************************************************************************/
int MP3GetNextFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo, unsigned char *buf) {
  MP3DecInfo *mp3DecInfo = (MP3DecInfo *) hMP3Decoder;

  if (!mp3DecInfo)
    return ERR_MP3_NULL_POINTER;

  if (UnpackFrameHeader(mp3DecInfo, buf) == -1 || mp3DecInfo->layer != 3)
    return ERR_MP3_INVALID_FRAMEHEADER;

  MP3GetLastFrameInfo(mp3DecInfo, mp3FrameInfo);

  return ERR_MP3_NONE;
}

/**************************************************************************************
 * Function:    MP3ClearBadFrame
 *
 * Description: zero out pcm buffer if error decoding MP3 frame
 *
 * Inputs:      mp3DecInfo struct with correct frame size parameters filled in
 *              pointer pcm output buffer
 *
 * Outputs:     zeroed out pcm buffer
 *
 * Return:      none
 **************************************************************************************/
static void MP3ClearBadFrame(MP3DecInfo *mp3DecInfo, short *outbuf) {
  int i;

  if (!mp3DecInfo)
    return;

  for (i = 0; i < mp3DecInfo->nGrans * mp3DecInfo->nGranSamps * mp3DecInfo->nChans; i++)
    outbuf[i] = 0;
}

/**************************************************************************************
 * Function:    MP3Decode
 *
 * Description: decode one frame of MP3 data
 *
 * Inputs:      valid MP3 decoder instance pointer (HMP3Decoder)
 *              double pointer to buffer of MP3 data (containing headers +
 *mainData) number of valid bytes remaining in inbuf pointer to outbuf, big
 *enough to hold one frame of decoded PCM samples flag indicating whether MP3
 *data is normal MPEG format (useSize = 0) or reformatted as "self-contained"
 *frames (useSize = 1)
 *
 * Outputs:     PCM data in outbuf, interleaved LRLRLR... if stereo
 *                number of output samples = nGrans * nGranSamps * nChans
 *              updated inbuf pointer, updated bytesLeft
 *
 * Return:      error code, defined in mp3dec.h (0 means no error, < 0 means
 *error)
 *
 * Notes:       switching useSize on and off between frames in the same stream
 *                is not supported (bit reservoir is not maintained if useSize
 *on)
 **************************************************************************************/
int MP3Decode(HMP3Decoder hMP3Decoder, unsigned char **inbuf, int *bytesLeft, short *outbuf, int useSize) {
  int offset, bitOffset, mainBits, gr, ch, fhBytes, siBytes, freeFrameBytes;
  int prevBitOffset, sfBlockBits, huffBlockBits;
  unsigned char *mainPtr;
  MP3DecInfo *mp3DecInfo = (MP3DecInfo *) hMP3Decoder;

  if (!mp3DecInfo)
    return ERR_MP3_NULL_POINTER;

  /* unpack frame header */
  fhBytes = UnpackFrameHeader(mp3DecInfo, *inbuf);
  if (fhBytes < 0)
    return ERR_MP3_INVALID_FRAMEHEADER; /* don't clear outbuf since we don't
                                           know size (failed to parse header) */
  *inbuf += fhBytes;

  /* unpack side info */
  siBytes = UnpackSideInfo(mp3DecInfo, *inbuf);
  if (siBytes < 0) {
    MP3ClearBadFrame(mp3DecInfo, outbuf);
    return ERR_MP3_INVALID_SIDEINFO;
  }
  *inbuf += siBytes;
  *bytesLeft -= (fhBytes + siBytes);

  /* if free mode, need to calculate bitrate and nSlots manually, based on frame
   * size */
  if (mp3DecInfo->bitrate == 0 || mp3DecInfo->freeBitrateFlag) {
    if (!mp3DecInfo->freeBitrateFlag) {
      /* first time through, need to scan for next sync word and figure out
       * frame size */
      mp3DecInfo->freeBitrateFlag = 1;
      mp3DecInfo->freeBitrateSlots = MP3FindFreeSync(*inbuf, *inbuf - fhBytes - siBytes, *bytesLeft);
      if (mp3DecInfo->freeBitrateSlots < 0) {
        MP3ClearBadFrame(mp3DecInfo, outbuf);
        return ERR_MP3_FREE_BITRATE_SYNC;
      }
      freeFrameBytes = mp3DecInfo->freeBitrateSlots + fhBytes + siBytes;
      mp3DecInfo->bitrate = (freeFrameBytes * mp3DecInfo->samprate * 8) / (mp3DecInfo->nGrans * mp3DecInfo->nGranSamps);
    }
    mp3DecInfo->nSlots = mp3DecInfo->freeBitrateSlots + CheckPadBit(mp3DecInfo); /* add pad byte, if required */
  }

  /* useSize != 0 means we're getting reformatted (RTP) packets (see RFC 3119)
   *  - calling function assembles "self-contained" MP3 frames by shifting any
   * main_data from the bit reservoir (in previous frames) to AFTER the sync
   * word and side info
   *  - calling function should set mainDataBegin to 0, and tell us exactly how
   * large this frame is (in bytesLeft)
   */
  if (useSize) {
    mp3DecInfo->nSlots = *bytesLeft;
    if (mp3DecInfo->mainDataBegin != 0 || mp3DecInfo->nSlots <= 0) {
      /* error - non self-contained frame, or missing frame (size <= 0), could
       * do loss concealment here */
      MP3ClearBadFrame(mp3DecInfo, outbuf);
      return ERR_MP3_INVALID_FRAMEHEADER;
    }

    /* can operate in-place on reformatted frames */
    mp3DecInfo->mainDataBytes = mp3DecInfo->nSlots;
    mainPtr = *inbuf;
    *inbuf += mp3DecInfo->nSlots;
    *bytesLeft -= (mp3DecInfo->nSlots);
  } else {
    /* out of data - assume last or truncated frame */
    if (mp3DecInfo->nSlots > *bytesLeft) {
      MP3ClearBadFrame(mp3DecInfo, outbuf);
      return ERR_MP3_INDATA_UNDERFLOW;
    }

    /* fill main data buffer with enough new data for this frame */
    if (mp3DecInfo->mainDataBytes >= mp3DecInfo->mainDataBegin) {
      /* adequate "old" main data available (i.e. bit reservoir) */
      memmove(mp3DecInfo->mainBuf, mp3DecInfo->mainBuf + mp3DecInfo->mainDataBytes - mp3DecInfo->mainDataBegin,
              mp3DecInfo->mainDataBegin);
      memcpy(mp3DecInfo->mainBuf + mp3DecInfo->mainDataBegin, *inbuf, mp3DecInfo->nSlots);

      mp3DecInfo->mainDataBytes = mp3DecInfo->mainDataBegin + mp3DecInfo->nSlots;
      *inbuf += mp3DecInfo->nSlots;
      *bytesLeft -= (mp3DecInfo->nSlots);
      mainPtr = mp3DecInfo->mainBuf;
    } else {
      /* not enough data in bit reservoir from previous frames (perhaps starting
       * in middle of file) */
      memcpy(mp3DecInfo->mainBuf + mp3DecInfo->mainDataBytes, *inbuf, mp3DecInfo->nSlots);
      mp3DecInfo->mainDataBytes += mp3DecInfo->nSlots;
      *inbuf += mp3DecInfo->nSlots;
      *bytesLeft -= (mp3DecInfo->nSlots);
      MP3ClearBadFrame(mp3DecInfo, outbuf);
      return ERR_MP3_MAINDATA_UNDERFLOW;
    }
  }
  bitOffset = 0;
  mainBits = mp3DecInfo->mainDataBytes * 8;

  /* decode one complete frame */
  for (gr = 0; gr < mp3DecInfo->nGrans; gr++) {
    for (ch = 0; ch < mp3DecInfo->nChans; ch++) {
      /* unpack scale factors and compute size of scale factor block */
      prevBitOffset = bitOffset;
      offset = UnpackScaleFactors(mp3DecInfo, mainPtr, &bitOffset, mainBits, gr, ch);

      sfBlockBits = 8 * offset - prevBitOffset + bitOffset;
      huffBlockBits = mp3DecInfo->part23Length[gr][ch] - sfBlockBits;
      mainPtr += offset;
      mainBits -= sfBlockBits;

      if (offset < 0 || mainBits < huffBlockBits) {
        MP3ClearBadFrame(mp3DecInfo, outbuf);
        return ERR_MP3_INVALID_SCALEFACT;
      }

      /* decode Huffman code words */
      prevBitOffset = bitOffset;
      offset = DecodeHuffman(mp3DecInfo, mainPtr, &bitOffset, huffBlockBits, gr, ch);
      if (offset < 0) {
        MP3ClearBadFrame(mp3DecInfo, outbuf);
        return ERR_MP3_INVALID_HUFFCODES;
      }

      mainPtr += offset;
      mainBits -= (8 * offset - prevBitOffset + bitOffset);
    }

    /* dequantize coefficients, decode stereo, reorder short blocks */
    if (Dequantize(mp3DecInfo, gr) < 0) {
      MP3ClearBadFrame(mp3DecInfo, outbuf);
      return ERR_MP3_INVALID_DEQUANTIZE;
    }

    /* alias reduction, inverse MDCT, overlap-add, frequency inversion */
    for (ch = 0; ch < mp3DecInfo->nChans; ch++) {
      if (IMDCT(mp3DecInfo, gr, ch) < 0) {
        MP3ClearBadFrame(mp3DecInfo, outbuf);
        return ERR_MP3_INVALID_IMDCT;
      }
    }

    /* subband transform - if stereo, interleaves pcm LRLRLR */
    if (Subband(mp3DecInfo, outbuf + gr * mp3DecInfo->nGranSamps * mp3DecInfo->nChans) < 0) {
      MP3ClearBadFrame(mp3DecInfo, outbuf);
      return ERR_MP3_INVALID_SUBBAND;
    }
  }
  return ERR_MP3_NONE;
}
#endif
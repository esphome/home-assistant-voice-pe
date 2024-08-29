#ifdef USE_ESP_IDF

#ifndef MP3_DECODER_H_
#define MP3_DECODER_H_

#include <stdlib.h>
#include <string.h>
#include <xtensa/config/core-isa.h>

#define ASSERT(x) /* do nothing */

/* determining MAINBUF_SIZE:
 *   max mainDataBegin = (2^9 - 1) bytes (since 9-bit offset) = 511
 *   max nSlots (concatenated with mainDataBegin bytes from before) = 1440 - 9 -
 * 4 + 1 = 1428 511 + 1428 = 1939, round up to 1940 (4-byte align)
 */
#define MAINBUF_SIZE 1940

#define MAX_NGRAN 2   /* max granules */
#define MAX_NCHAN 2   /* max channels */
#define MAX_NSAMP 576 /* max samples per channel, per granule */

/* map to 0,1,2 to make table indexing easier */
typedef enum { MPEG1 = 0, MPEG2 = 1, MPEG25 = 2 } MPEGVersion;

#define MAX_SCFBD 4 /* max scalefactor bands per channel */
#define NGRANS_MPEG1 2
#define NGRANS_MPEG2 1

/* 11-bit syncword if MPEG 2.5 extensions are enabled */
/*
#define	SYNCWORDH		0xff
#define	SYNCWORDL		0xe0
*/

/* 12-bit syncword if MPEG 1,2 only are supported */
#define SYNCWORDH 0xff
#define SYNCWORDL 0xf0

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

typedef long long Word64;

static __inline Word64 MADD64(Word64 sum64, int x, int y) { return (sum64 + ((long long) x * y)); }

static __inline int MULSHIFT32(int x, int y) {
  /* important rules for smull RdLo, RdHi, Rm, Rs:
   *     RdHi and Rm can't be the same register
   *     RdLo and Rm can't be the same register
   *     RdHi and RdLo can't be the same register
   * Note: Rs determines early termination (leading sign bits) so if you want to
   * specify which operand is Rs, put it in the SECOND argument (y) For inline
   * assembly, x and y are not assumed to be R0, R1 so it shouldn't matter which
   * one is returned. (If this were a function call, returning y (R1) would
   *   require an extra "mov r0, r1")
   */
  int ret;
  asm volatile("mulsh %0, %1, %2" : "=r"(ret) : "r"(x), "r"(y));
  return ret;
}

static __inline int FASTABS(int x) {
  int ret;
  asm volatile("abs %0, %1" : "=r"(ret) : "r"(x));
  return ret;
}

static __inline Word64 SAR64(Word64 x, int n) { return x >> n; }

static __inline int CLZ(int x) { return __builtin_clz(x); }

/* clip to range [-2^n, 2^n - 1] */
#define CLIP_2N(y, n) \
  { \
    int sign = (y) >> 31; \
    if (sign != (y) >> (n)) { \
      (y) = sign ^ ((1 << (n)) - 1); \
    } \
  }

#define SIBYTES_MPEG1_MONO 17
#define SIBYTES_MPEG1_STEREO 32
#define SIBYTES_MPEG2_MONO 9
#define SIBYTES_MPEG2_STEREO 17

/* number of fraction bits for pow43Tab (see comments there) */
#define POW43_FRACBITS_LOW 22
#define POW43_FRACBITS_HIGH 12

#define DQ_FRACBITS_OUT 25 /* number of fraction bits in output of dequant */
#define IMDCT_SCALE 2      /* additional scaling (by sqrt(2)) for fast IMDCT36 */

#define HUFF_PAIRTABS 32
#define BLOCK_SIZE 18
#define NBANDS 32
#define MAX_REORDER_SAMPS ((192 - 126) * 3) /* largest critical band for short blocks (see sfBandTable) */
#define VBUF_LENGTH (17 * 2 * NBANDS)       /* for double-sized vbuf FIFO */

/* map these to the corresponding 2-bit values in the frame header */
typedef enum {
  Stereo = 0x00, /* two independent channels, but L and R frames might have
                    different # of bits */
  Joint = 0x01,  /* coupled channels - layer III: mix of M-S and intensity,
                    Layers I/II: intensity and direct coding only */
  Dual = 0x02,   /* two independent channels, L and R always have exactly 1/2 the
                    total bitrate */
  Mono = 0x03    /* one channel */
} StereoMode;

typedef struct _SFBandTable {
  short l[23];
  short s[14];
} SFBandTable;

typedef struct _BitStreamInfo {
  unsigned char *bytePtr;
  unsigned int iCache;
  int cachedBits;
  int nBytes;
} BitStreamInfo;

typedef struct _FrameHeader {
  MPEGVersion ver;  /* version ID */
  int layer;        /* layer index (1, 2, or 3) */
  int crc;          /* CRC flag: 0 = disabled, 1 = enabled */
  int brIdx;        /* bitrate index (0 - 15) */
  int srIdx;        /* sample rate index (0 - 2) */
  int paddingBit;   /* padding flag: 0 = no padding, 1 = single pad byte */
  int privateBit;   /* unused */
  StereoMode sMode; /* mono/stereo mode */
  int modeExt;      /* used to decipher joint stereo mode */
  int copyFlag;     /* copyright flag: 0 = no, 1 = yes */
  int origFlag;     /* original flag: 0 = copy, 1 = original */
  int emphasis;     /* deemphasis mode */
  int CRCWord;      /* CRC word (16 bits, 0 if crc not enabled) */

  const SFBandTable *sfBand;
} FrameHeader;

typedef struct _SideInfoSub {
  int part23Length;      /* number of bits in main data */
  int nBigvals;          /* 2x this = first set of Huffman cw's (maximum amplitude can be
                            > 1) */
  int globalGain;        /* overall gain for dequantizer */
  int sfCompress;        /* unpacked to figure out number of bits in scale factors */
  int winSwitchFlag;     /* window switching flag */
  int blockType;         /* block type */
  int mixedBlock;        /* 0 = regular block (all short or long), 1 = mixed block */
  int tableSelect[3];    /* index of Huffman tables for the big values regions */
  int subBlockGain[3];   /* subblock gain offset, relative to global gain */
  int region0Count;      /* 1+region0Count = num scale factor bands in first region
                            of bigvals */
  int region1Count;      /* 1+region1Count = num scale factor bands in second region
                            of bigvals */
  int preFlag;           /* for optional high frequency boost */
  int sfactScale;        /* scaling of the scalefactors */
  int count1TableSelect; /* index of Huffman table for quad codewords */
} SideInfoSub;

typedef struct _SideInfo {
  int mainDataBegin;
  int privateBits;
  int scfsi[MAX_NCHAN][MAX_SCFBD]; /* 4 scalefactor bands per channel */

  SideInfoSub sis[MAX_NGRAN][MAX_NCHAN];
} SideInfo;

typedef struct {
  int cbType;    /* pure long = 0, pure short = 1, mixed = 2 */
  int cbEndS[3]; /* number nonzero short cb's, per subbblock */
  int cbEndSMax; /* max of cbEndS[] */
  int cbEndL;    /* number nonzero long cb's  */
} CriticalBandInfo;

typedef struct _DequantInfo {
  int workBuf[MAX_REORDER_SAMPS];  /* workbuf for reordering short blocks */
  CriticalBandInfo cbi[MAX_NCHAN]; /* filled in dequantizer, used in joint
                                      stereo reconstruction */
} DequantInfo;

typedef struct _HuffmanInfo {
  int huffDecBuf[MAX_NCHAN][MAX_NSAMP]; /* used both for decoded Huffman values
                                           and dequantized coefficients */
  int nonZeroBound[MAX_NCHAN];          /* number of coeffs in huffDecBuf[ch] which can
                                           be > 0 */
  int gb[MAX_NCHAN];                    /* minimum number of guard bits in huffDecBuf[ch] */
} HuffmanInfo;

typedef enum _HuffTabType { noBits, oneShot, loopNoLinbits, loopLinbits, quadA, quadB, invalidTab } HuffTabType;

typedef struct _HuffTabLookup {
  int linBits;
  HuffTabType tabType;
} HuffTabLookup;

typedef struct _IMDCTInfo {
  int outBuf[MAX_NCHAN][BLOCK_SIZE][NBANDS]; /* output of IMDCT */
  int overBuf[MAX_NCHAN][MAX_NSAMP / 2];     /* overlap-add buffer (by symmetry,
                                                only need 1/2 size) */
  int numPrevIMDCT[MAX_NCHAN];               /* how many IMDCT's calculated in this channel on
                                                prev. granule */
  int prevType[MAX_NCHAN];
  int prevWinSwitch[MAX_NCHAN];
  int gb[MAX_NCHAN];
} IMDCTInfo;

typedef struct _BlockCount {
  int nBlocksLong;
  int nBlocksTotal;
  int nBlocksPrev;
  int prevType;
  int prevWinSwitch;
  int currWinSwitch;
  int gbIn;
  int gbOut;
} BlockCount;

/* max bits in scalefactors = 5, so use char's to save space */
typedef struct _ScaleFactorInfoSub {
  char l[23];    /* [band] */
  char s[13][3]; /* [band][window] */
} ScaleFactorInfoSub;

/* used in MPEG 2, 2.5 intensity (joint) stereo only */
typedef struct _ScaleFactorJS {
  int intensityScale;
  int slen[4];
  int nr[4];
} ScaleFactorJS;

typedef struct _ScaleFactorInfo {
  ScaleFactorInfoSub sfis[MAX_NGRAN][MAX_NCHAN];
  ScaleFactorJS sfjs;
} ScaleFactorInfo;

/* NOTE - could get by with smaller vbuf if memory is more important than speed
 *  (in Subband, instead of replicating each block in FDCT32 you would do a
 * memmove on the last 15 blocks to shift them down one, a hardware style FIFO)
 */
typedef struct _SubbandInfo {
  int vbuf[MAX_NCHAN * VBUF_LENGTH]; /* vbuf for fast DCT-based synthesis PQMF - double size
                                        for speed (no modulo indexing) */
  int vindex;                        /* internal index for tracking position in vbuf */
} SubbandInfo;

/* bitstream.c */
void SetBitstreamPointer(BitStreamInfo *bsi, int nBytes, unsigned char *buf);
unsigned int GetBits(BitStreamInfo *bsi, int nBits);
int CalcBitsUsed(BitStreamInfo *bsi, unsigned char *startBuf, int startOffset);

/* dequant.c, dqchan.c, stproc.c */
int DequantChannel(int *sampleBuf, int *workBuf, int *nonZeroBound, FrameHeader *fh, SideInfoSub *sis,
                   ScaleFactorInfoSub *sfis, CriticalBandInfo *cbi);
void MidSideProc(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, int mOut[2]);
void IntensityProcMPEG1(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, FrameHeader *fh, ScaleFactorInfoSub *sfis,
                        CriticalBandInfo *cbi, int midSideFlag, int mixFlag, int mOut[2]);
void IntensityProcMPEG2(int x[MAX_NCHAN][MAX_NSAMP], int nSamps, FrameHeader *fh, ScaleFactorInfoSub *sfis,
                        CriticalBandInfo *cbi, ScaleFactorJS *sfjs, int midSideFlag, int mixFlag, int mOut[2]);

/* dct32.c */
// about 1 ms faster in RAM, but very large
void FDCT32(int *x, int *d, int offset, int oddBlock,
            int gb);  // __attribute__ ((section (".data")));

/* hufftabs.c */
extern const HuffTabLookup huffTabLookup[HUFF_PAIRTABS];
extern const int huffTabOffset[HUFF_PAIRTABS];
extern const unsigned short huffTable[];
extern const unsigned char quadTable[64 + 16];
extern const int quadTabOffset[2];
extern const int quadTabMaxBits[2];

void PolyphaseMono(short *pcm, int *vbuf, const int *coefBase);
void PolyphaseStereo(short *pcm, int *vbuf, const int *coefBase);

/* trigtabs.c */
extern const uint32_t imdctWin[4][36];
extern const int ISFMpeg1[2][7];
extern const int ISFMpeg2[2][2][16];
extern const int ISFIIP[2][2];
extern const uint32_t csa[8][2];
extern const int coef32[31];
extern const uint32_t polyCoef[264];

typedef struct _MP3DecInfo {
  /* pointers to platform-specific data structures */
  void *FrameHeaderPS;
  void *SideInfoPS;
  void *ScaleFactorInfoPS;
  void *HuffmanInfoPS;
  void *DequantInfoPS;
  void *IMDCTInfoPS;
  void *SubbandInfoPS;

  /* buffer which must be large enough to hold largest possible main_data
   * section */
  unsigned char mainBuf[MAINBUF_SIZE];

  /* special info for "free" bitrate files */
  int freeBitrateFlag;
  int freeBitrateSlots;

  /* user-accessible info */
  int bitrate;
  int nChans;
  int samprate;
  int nGrans;     /* granules per frame */
  int nGranSamps; /* samples per granule */
  int nSlots;
  int layer;
  MPEGVersion version;

  int mainDataBegin;
  int mainDataBytes;

  int part23Length[MAX_NGRAN][MAX_NCHAN];

} MP3DecInfo;

MP3DecInfo *AllocateBuffers(void);
void FreeBuffers(MP3DecInfo *mp3DecInfo);
int CheckPadBit(MP3DecInfo *mp3DecInfo);
int UnpackFrameHeader(MP3DecInfo *mp3DecInfo, unsigned char *buf);
int UnpackSideInfo(MP3DecInfo *mp3DecInfo, unsigned char *buf);
int DecodeHuffman(MP3DecInfo *mp3DecInfo, unsigned char *buf, int *bitOffset, int huffBlockBits, int gr, int ch);
int Dequantize(MP3DecInfo *mp3DecInfo, int gr);
int IMDCT(MP3DecInfo *mp3DecInfo, int gr, int ch);
int UnpackScaleFactors(MP3DecInfo *mp3DecInfo, unsigned char *buf, int *bitOffset, int bitsAvail, int gr, int ch);
int Subband(MP3DecInfo *mp3DecInfo, short *pcmBuf);

extern const int samplerateTab[3][3];
extern const short bitrateTab[3][3][15];
extern const short samplesPerFrameTab[3][3];
extern const short bitsPerSlotTab[3];
extern const short sideBytesTab[3][2];
extern const short slotTab[3][3][15];
extern const SFBandTable sfBandTable[3][3];

typedef void *HMP3Decoder;

enum {
  ERR_MP3_NONE = 0,
  ERR_MP3_INDATA_UNDERFLOW = -1,
  ERR_MP3_MAINDATA_UNDERFLOW = -2,
  ERR_MP3_FREE_BITRATE_SYNC = -3,
  ERR_MP3_OUT_OF_MEMORY = -4,
  ERR_MP3_NULL_POINTER = -5,
  ERR_MP3_INVALID_FRAMEHEADER = -6,
  ERR_MP3_INVALID_SIDEINFO = -7,
  ERR_MP3_INVALID_SCALEFACT = -8,
  ERR_MP3_INVALID_HUFFCODES = -9,
  ERR_MP3_INVALID_DEQUANTIZE = -10,
  ERR_MP3_INVALID_IMDCT = -11,
  ERR_MP3_INVALID_SUBBAND = -12,

  ERR_UNKNOWN = -9999
};

typedef struct _MP3FrameInfo {
  int bitrate;
  int nChans;
  int samprate;
  int bitsPerSample;
  int outputSamps;
  int layer;
  int version;
} MP3FrameInfo;

/* public API */
HMP3Decoder MP3InitDecoder(void);
void MP3FreeDecoder(HMP3Decoder hMP3Decoder);
int MP3Decode(HMP3Decoder hMP3Decoder, unsigned char **inbuf, int *bytesLeft, short *outbuf, int useSize);

void MP3GetLastFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo);
int MP3GetNextFrameInfo(HMP3Decoder hMP3Decoder, MP3FrameInfo *mp3FrameInfo, unsigned char *buf);
int MP3FindSyncWord(unsigned char *buf, int nBytes);

#endif  // MP3_DECODER_H_
#endif
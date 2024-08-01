#pragma once

#ifdef USE_ESP_IDF

#include "flac_decoder.h"
#include "wav_decoder.h"
#include "mp3_decoder.h"

#include "esphome/components/media_player/media_player.h"
#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace nabu {

enum class AudioDecoderState : uint8_t {
  INITIALIZED = 0,
  DECODING,
  FINISHED,
  FAILED,
};

// Only used within the AudioDecoder class; conveys the state of the particular file type decoder
enum class FileDecoderState : uint8_t {
  MORE_TO_PROCESS,
  IDLE,
  POTENTIALLY_FAILED,
  FAILED,
  END_OF_FILE,
};

class AudioDecoder {
 public:
  AudioDecoder(esphome::RingBuffer *input_ring_buffer, esphome::RingBuffer *output_ring_buffer, size_t internal_buffer_size);
  ~AudioDecoder();

  void start(media_player::MediaFileType media_file_type);

  AudioDecoderState decode(bool stop_gracefully);

  const optional<uint8_t> &get_channels() const { return this->channels_; }
  const optional<uint8_t> &get_sample_depth() const { return this->sample_depth_; }
  const optional<uint32_t> &get_sample_rate() const { return this->sample_rate_; }

 protected:
  FileDecoderState decode_wav_();
  FileDecoderState decode_mp3_();
  FileDecoderState decode_flac_();

  esphome::RingBuffer *input_ring_buffer_;
  esphome::RingBuffer *output_ring_buffer_;
  size_t internal_buffer_size_;

  uint8_t *input_buffer_;
  uint8_t *input_buffer_current_;
  size_t input_buffer_length_;

  uint8_t *output_buffer_;
  uint8_t *output_buffer_current_;
  size_t output_buffer_length_;

  HMP3Decoder mp3_decoder_;

  wav_decoder::WAVDecoder *wav_decoder_{nullptr};
  size_t wav_bytes_left_;

  flac::FLACDecoder *flac_decoder_{nullptr};

  media_player::MediaFileType media_file_type_{media_player::MediaFileType::NONE};
  optional<uint8_t> channels_;
  optional<uint8_t> sample_depth_;
  optional<uint32_t> sample_rate_;

  size_t potentially_failed_count_{0};
  bool end_of_file_{false};
};
}  // namespace nabu
}  // namespace esphome

#endif
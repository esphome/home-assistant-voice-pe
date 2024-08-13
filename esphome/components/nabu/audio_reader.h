#pragma once

#ifdef USE_ESP_IDF

#include "esphome/components/media_player/media_player.h"
#include "esphome/core/ring_buffer.h"

#include <esp_http_client.h>

namespace esphome {
namespace nabu {

enum class AudioReaderState : uint8_t {
  INITIALIZED = 0,
  READING,
  FINISHED,
  FAILED,
};

class AudioReader {
 public:
  AudioReader(esphome::RingBuffer *output_ring_buffer, size_t transfer_buffer_size);
  ~AudioReader();

  media_player::MediaFileType start(const std::string &uri);
  media_player::MediaFileType start(media_player::MediaFile *media_file);

  AudioReaderState read();

 protected:
  esp_err_t allocate_buffers_();

  AudioReaderState file_read_();
  AudioReaderState http_read_();

  void cleanup_connection_();

  esphome::RingBuffer *output_ring_buffer_;
  uint8_t *transfer_buffer_{nullptr};
  size_t transfer_buffer_size_;

  esp_http_client_handle_t client_{nullptr};

  media_player::MediaFile *current_media_file_{nullptr};
  size_t media_file_bytes_left_;
  const uint8_t *media_file_data_current_;
};
}  // namespace nabu
}  // namespace esphome

#endif
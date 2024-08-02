#ifdef USE_ESP_IDF

#include "audio_reader.h"

#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace nabu {

AudioReader::AudioReader(esphome::RingBuffer *output_ring_buffer, size_t transfer_buffer_size) {
  this->output_ring_buffer_ = output_ring_buffer;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->transfer_buffer_ = allocator.allocate(transfer_buffer_size);
  this->transfer_buffer_size_ = transfer_buffer_size;
}

AudioReader::~AudioReader() {
  if (this->transfer_buffer_ != nullptr) {
    ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
    allocator.deallocate(this->transfer_buffer_, this->transfer_buffer_size_);
  }

  this->cleanup_connection_();
}

media_player::MediaFileType AudioReader::start(media_player::MediaFile *media_file) {
  this->current_media_file_ = media_file;

  this->media_file_data_current_ = media_file->data;
  this->media_file_bytes_left_ = media_file->length;

  return media_file->file_type;
}

media_player::MediaFileType AudioReader::start(const std::string &uri) {
  this->cleanup_connection_();

  if (uri.empty()) {
    return media_player::MediaFileType::NONE;
  }

  esp_http_client_config_t config = {
      .url = uri.c_str(),
      .cert_pem = nullptr,
      .disable_auto_redirect = false,
      .max_redirection_count = 10,
  };
  this->client_ = esp_http_client_init(&config);

  if (this->client_ == nullptr) {
    printf("Failed to initialize HTTP connection");
    return media_player::MediaFileType::NONE;
  }

  esp_err_t err;
  if ((err = esp_http_client_open(this->client_, 0)) != ESP_OK) {
    printf("Failed to open HTTP connection");
    this->cleanup_connection_();
    return media_player::MediaFileType::NONE;
  }

  int content_length = esp_http_client_fetch_headers(this->client_);

  // TODO: Figure out how to handle this better! Music Assistant streams don't send a content length
  // if (content_length <= 0) {
  //   printf("Fialed to get content length");
  //   this->cleanup_connection_(client);
  //   return media_player::MediaFileType::NONE;
  // }

  char url[500];
  if (esp_http_client_get_url(this->client_, url, 500) != ESP_OK) {
    this->cleanup_connection_();
    return media_player::MediaFileType::NONE;
  }

  std::string url_string = url;

  if (str_endswith(url_string, ".wav")) {
    return media_player::MediaFileType::WAV;
  } else if (str_endswith(url_string, ".mp3")) {
    return media_player::MediaFileType::MP3;
  } else if (str_endswith(url_string, ".flac")) {
    return media_player::MediaFileType::FLAC;
  }

  return media_player::MediaFileType::NONE;
}

AudioReaderState AudioReader::read() {
  if (this->client_ != nullptr) {
    return this->http_read_();
  } else if (this->current_media_file_ != nullptr) {
    return this->file_read_();
  }

  return AudioReaderState::INITIALIZED;
}

AudioReaderState AudioReader::file_read_() {
  if (this->media_file_bytes_left_ > 0) {
    size_t bytes_to_write = std::min(this->media_file_bytes_left_, this->output_ring_buffer_->free());

    if (bytes_to_write == 0) {
      return AudioReaderState::READING;
    }

    size_t bytes_written = this->output_ring_buffer_->write((void *) this->media_file_data_current_, bytes_to_write);
    this->media_file_bytes_left_ -= bytes_written;
    this->media_file_data_current_ += bytes_written;

    return AudioReaderState::READING;
  }
  return AudioReaderState::FINISHED;
}

AudioReaderState AudioReader::http_read_() {
  size_t bytes_to_read = this->output_ring_buffer_->free();

  if (bytes_to_read == 0) {
    return AudioReaderState::READING;
  }

  int received_len = esp_http_client_read(this->client_, (char *) this->transfer_buffer_, bytes_to_read);

  if (received_len > 0) {
    this->output_ring_buffer_->write((void *) this->transfer_buffer_, received_len);
  } else if (received_len < 0) {
    // TODO: Error situation. Should we mark failed..?
  }

  if (esp_http_client_is_complete_data_received(this->client_)) {
    this->cleanup_connection_();
    return AudioReaderState::FINISHED;
  }

  return AudioReaderState::READING;
}

void AudioReader::cleanup_connection_() {
  if (this->client_ != nullptr) {
    esp_http_client_close(this->client_);
    esp_http_client_cleanup(this->client_);
    this->client_ = nullptr;
  }
}

}  // namespace nabu
}  // namespace esphome

#endif
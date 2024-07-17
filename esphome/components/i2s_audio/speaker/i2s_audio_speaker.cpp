#include "i2s_audio_speaker.h"

#ifdef USE_ESP32

#include <driver/i2s.h>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "esphome/core/ring_buffer.h"

// #include "esp_dsp.h" // temporarily added for volume reduction

// Major TODOs:
//  - optimize buffer sizes/memory used for each task
//  - handle stereo audio samples

namespace esphome {
namespace i2s_audio {

static const size_t SAMPLE_RATE_HZ = 16000;    // 16 kHz
static const size_t RING_BUFFER_LENGTH = 200;  // 0.064 seconds
static const size_t RING_BUFFER_SIZE = SAMPLE_RATE_HZ / 1000 * RING_BUFFER_LENGTH;
static const size_t QUEUE_COUNT = 20;
static const size_t DMA_BUFFER_COUNT = 4;
static const size_t DMA_BUFFER_SIZE = 512;
static const size_t BUFFER_SIZE = DMA_BUFFER_COUNT*DMA_BUFFER_SIZE;

static const char *const TAG = "i2s_audio.speaker";

void I2SAudioSpeaker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Speaker...");

  this->play_command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));
  this->play_event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));

  this->input_ring_buffer_ = RingBuffer::create(RING_BUFFER_SIZE * sizeof(int16_t));
  if (this->input_ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate ring buffer");
    this->mark_failed();
    return;
  }
}

void I2SAudioSpeaker::player_task(void *params) {
  I2SAudioSpeaker *this_speaker = (I2SAudioSpeaker *) params;

  TaskEvent event;
  CommandEvent command_event;

  event.type = TaskEventType::STARTING;
  xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

  ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  int16_t *buffer = allocator.allocate(2*BUFFER_SIZE);
  int16_t *temp_buffer =
      allocator.allocate(BUFFER_SIZE);  // only adding this to temporarily hardcode a constant volume reduction

  if ((buffer == nullptr) || (temp_buffer == nullptr)) {
    event.type = TaskEventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

    event.type = TaskEventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  i2s_driver_config_t config = {
      .mode = (i2s_mode_t) (this_speaker->parent_->get_i2s_mode() | I2S_MODE_TX),
      .sample_rate = 16000,
      .bits_per_sample = this_speaker->bits_per_sample_,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = DMA_BUFFER_COUNT,
      .dma_buf_len = DMA_BUFFER_SIZE,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = I2S_PIN_NO_CHANGE,
      .mclk_multiple = I2S_MCLK_MULTIPLE_DEFAULT,
      .bits_per_chan = I2S_BITS_PER_CHAN_DEFAULT,
#if SOC_I2S_SUPPORTS_TDM
      .chan_mask = (i2s_channel_t) (I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1),
      .total_chan = 2,
      .left_align = false,
      .big_edin = false,
      .bit_order_msb = false,
      .skip_msk = false,
#endif
  };
#if SOC_I2S_SUPPORTS_DAC
  if (this_speaker->internal_dac_mode_ != I2S_DAC_CHANNEL_DISABLE) {
    config.mode = (i2s_mode_t) (config.mode | I2S_MODE_DAC_BUILT_IN);
  }
#endif

  esp_err_t err = i2s_driver_install(this_speaker->parent_->get_port(), &config, 0, nullptr);
  if (err != ESP_OK) {
    event.type = TaskEventType::WARNING;
    event.err = err;
    xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

    event.type = TaskEventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

#if SOC_I2S_SUPPORTS_DAC
  if (this_speaker->internal_dac_mode_ == I2S_DAC_CHANNEL_DISABLE) {
#endif
    i2s_pin_config_t pin_config = this_speaker->parent_->get_pin_config();
    pin_config.data_out_num = this_speaker->dout_pin_;

    err = i2s_set_pin(this_speaker->parent_->get_port(), &pin_config);
#if SOC_I2S_SUPPORTS_DAC
  } else {
    err = i2s_set_dac_mode(this_speaker->internal_dac_mode_);
  }
#endif

  if (err != ESP_OK) {
    event.type = TaskEventType::WARNING;
    event.err = err;
    xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

    event.type = TaskEventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  // Assumes incoming audio stream is mono channel 16000 Hz
  // TODO: Move everything to stereo streams. Mono to stereo conversion should happen before being written to the
  // speaker
  // uint32_t bits_cfg = (this_speaker->bits_per_sample_ << 16) | this_speaker->bits_per_sample_;
  // err = i2s_set_clk(this_speaker->parent_->get_port(), 16000, bits_cfg, I2S_CHANNEL_MONO);

  event.type = TaskEventType::STARTED;
  xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

  while (true) {
    if (xQueueReceive(this_speaker->play_command_queue_, &command_event, (0 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.stop) {
        // Stop signal from main thread
        break;
      }
    }

    size_t delay_ms = 10;
    size_t bytes_to_read = DMA_BUFFER_SIZE*sizeof(int16_t);
    // if (event.type != TaskEventType::RUNNING) {
    //   // Fill the entire DMA buffer if there is audio being outputed
    //   bytes_to_read = DMA_BUFFER_COUNT*DMA_BUFFER_SIZE*sizeof(int16_t);
    // }

    size_t bytes_read = 0;
    if (this_speaker->combine_streamer_ == nullptr) {
      bytes_read = this_speaker->input_ring_buffer_->read((void *) buffer,bytes_to_read,
                                                          (delay_ms / portTICK_PERIOD_MS));
    } else {
      bytes_read = this_speaker->combine_streamer_->read((uint8_t *) buffer, bytes_to_read,
                                                         (delay_ms / portTICK_PERIOD_MS));
    }

    if (bytes_read > 0) {
      // its too loud for constant testing! this does lower the quality quite a bit though...
      // int16_t volume_reduction = (int16_t) (0.5f * std::pow(2, 15));
      // dsps_mulc_s16_ae32(buffer, temp_buffer,bytes_read/sizeof(int16_t), volume_reduction, 1, 1);
      // std::memcpy((void *) buffer, (void *) temp_buffer, bytes_read/sizeof(int16_t));

      // Copy mono audio samples into both channels
      for (int i = bytes_read/2-1; i >= 0; --i) {
        buffer[2*i] = buffer[i];
        buffer[2*i+1] = buffer[i];
      }

      size_t bytes_written;
      if (this_speaker->bits_per_sample_ == I2S_BITS_PER_SAMPLE_16BIT) {
        i2s_write(this_speaker->parent_->get_port(), buffer, 2*bytes_read, &bytes_written, portMAX_DELAY);
      } else {
        i2s_write_expand(this_speaker->parent_->get_port(), buffer, 2*bytes_read, I2S_BITS_PER_SAMPLE_16BIT,
                         this_speaker->bits_per_sample_, &bytes_written, portMAX_DELAY);
      }

      if (bytes_written != 2*bytes_read) {
        event.type = TaskEventType::WARNING;
        event.err = ESP_ERR_TIMEOUT;  // TODO: probably not the correct error...
        xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);
      }
      event.type = TaskEventType::RUNNING;
      xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);
    } else {
      i2s_zero_dma_buffer(this_speaker->parent_->get_port());

      event.type = TaskEventType::IDLE;
      xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);
    }
  }
  i2s_zero_dma_buffer(this_speaker->parent_->get_port());

  event.type = TaskEventType::STOPPING;
  xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

  allocator.deallocate(buffer, BUFFER_SIZE);
  i2s_stop(this_speaker->parent_->get_port());
  i2s_driver_uninstall(this_speaker->parent_->get_port());

  event.type = TaskEventType::STOPPED;
  xQueueSend(this_speaker->play_event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void I2SAudioSpeaker::start() {
  if (this->is_failed())
    return;
  if (this->state_ == speaker::STATE_RUNNING)
    return;  // Already running
  this->state_ = speaker::STATE_STARTING;
}

void I2SAudioSpeaker::start_() {
  if (!this->parent_->try_lock()) {
    return;  // Waiting for another i2s component to return lock
  }

  xTaskCreate(I2SAudioSpeaker::player_task, "speaker_task", 4096, (void *) this, 23, &this->player_task_handle_);
}

void I2SAudioSpeaker::stop() {
  if (this->state_ == speaker::STATE_STOPPED || this->is_failed())
    return;
  if (this->state_ == speaker::STATE_STARTING) {
    this->state_ = speaker::STATE_STOPPED;
    return;
  }
  this->state_ = speaker::STATE_STOPPING;
}

void I2SAudioSpeaker::stop_() {
  CommandEvent event;
  event.stop = true;
  xQueueSendToFront(this->play_command_queue_, &event, portMAX_DELAY);
}

void I2SAudioSpeaker::loop() {
  this->watch_();
  switch (this->state_) {
    case speaker::STATE_STARTING:
      this->start_();
      break;
    case speaker::STATE_RUNNING:
      // Do we want to implement data callbacks?
      break;
    case speaker::STATE_STOPPING:
      this->stop_();
      break;
    case speaker::STATE_STOPPED:
      break;
  }
}

void I2SAudioSpeaker::watch_() {
  TaskEvent event;
  while (xQueueReceive(this->play_event_queue_, &event, 0)) {
    switch (event.type) {
      case TaskEventType::STARTING:
        ESP_LOGD(TAG, "Starting I2S Audio Speaker");
        break;
      case TaskEventType::STARTED:
        ESP_LOGD(TAG, "Started I2S Audio Speaker");
        break;
      case TaskEventType::IDLE:
        this->is_playing_ = false;
        break;
      case TaskEventType::RUNNING:
        this->is_playing_ = true;
        this->status_clear_warning();
        break;
      case TaskEventType::STOPPING:
        ESP_LOGD(TAG, "Stopping I2S Audio Speaker");
        break;
      case TaskEventType::STOPPED:
        this->state_ = speaker::STATE_STOPPED;

        vTaskDelete(this->player_task_handle_);
        this->player_task_handle_ = nullptr;
        this->parent_->unlock();

        this->input_ring_buffer_->reset();
        xQueueReset(this->play_event_queue_);
        xQueueReset(this->play_command_queue_);

        ESP_LOGD(TAG, "Stopped I2S Audio Speaker");
        break;
      case TaskEventType::WARNING:
        ESP_LOGW(TAG, "Error writing to I2S: %s", esp_err_to_name(event.err));
        this->status_set_warning();
        break;
    }
  }
}

// Probably broken...
size_t I2SAudioSpeaker::play(const uint8_t *data, size_t length) {
  // TODO: Remove... not safe if called by multiple components; use write instead (drops backwards compatibility
  // though...)
  size_t remaining = length;
  size_t index = 0;
  while (remaining > 0) {
    size_t to_send_length = std::min(remaining, BUFFER_SIZE);
    size_t written = this->write(data + index, remaining);
    remaining -= written;
    index += written;
    delay(10);
  }
  return index;
}

size_t I2SAudioSpeaker::write(const uint8_t *data, size_t length) {
  // TODO: Protect against multiple components attempting from calling this at the same
  if (this->state_ != speaker::STATE_RUNNING && this->state_ != speaker::STATE_STARTING) {
    this->start();
  }
  size_t free_space = this->input_ring_buffer_->free();
  size_t how_much_to_write = std::min(length, free_space);
  size_t written = 0;
  if (how_much_to_write > 0) {
    written = this->input_ring_buffer_->write((void *) data, how_much_to_write);
  }

  return written;
}

bool I2SAudioSpeaker::has_buffered_data() const { return (this->input_ring_buffer_->available() > 0); }

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32

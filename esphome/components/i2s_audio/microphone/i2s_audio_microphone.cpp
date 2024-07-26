#include "i2s_audio_microphone.h"

#ifdef USE_ESP32

#include <driver/i2s.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/ring_buffer.h"

namespace esphome {
namespace i2s_audio {

static const size_t SAMPLE_RATE_HZ = 16000;  // 16 kHz
static const size_t RING_BUFFER_LENGTH = 64;      // 0.064 seconds
static const size_t RING_BUFFER_SIZE = SAMPLE_RATE_HZ / 1000 * RING_BUFFER_LENGTH;
static const size_t BUFFER_SIZE = 512;
static const size_t BUFFER_COUNT = 10;

// TODO: 
//   - Determine optimal buffer sizes (dma included)
//   - Determine appropriate timeout durations for FreeRTOS operations

static const char *const TAG = "i2s_audio.microphone";

void I2SAudioMicrophone::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Microphone...");
#if SOC_I2S_SUPPORTS_ADC
  if (this->adc_) {
    if (this->parent_->get_port() != I2S_NUM_0) {
      ESP_LOGE(TAG, "Internal ADC only works on I2S0!");
      this->mark_failed();
      return;
    }
  } else
#endif
      if (this->pdm_) {
    if (this->parent_->get_port() != I2S_NUM_0) {
      ESP_LOGE(TAG, "PDM only works on I2S0!");
      this->mark_failed();
      return;
    }
  }

  this->command_queue_ = xQueueCreate(BUFFER_COUNT, sizeof(CommandEvent));
  this->event_queue_ = xQueueCreate(BUFFER_COUNT, sizeof(TaskEvent));
  this->output_ring_buffer_ = RingBuffer::create(RING_BUFFER_SIZE * sizeof(int16_t));
  if (this->output_ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate ring buffer");
    this->mark_failed();
    return;
  }
}

void I2SAudioMicrophone::read_task_(void *params) {
  I2SAudioMicrophone *this_microphone = (I2SAudioMicrophone *) params;

  TaskEvent event;
  CommandEvent command_event;
  
  event.type = TaskEventType::STARTING;
  xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

  ExternalRAMAllocator<int32_t> allocator(ExternalRAMAllocator<int32_t>::ALLOW_FAILURE);
  int32_t *buffer = allocator.allocate(BUFFER_SIZE);

  if (buffer == nullptr) {
    event.type = TaskEventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

    event.type = TaskEventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  i2s_driver_config_t config = {
      .mode = (i2s_mode_t) (this_microphone->parent_->get_i2s_mode() | I2S_MODE_RX),
      .sample_rate = this_microphone->sample_rate_,
      .bits_per_sample = this_microphone->bits_per_sample_,
      .channel_format = this_microphone->channel_,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = this_microphone->use_apll_,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
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

  esp_err_t err;

#if SOC_I2S_SUPPORTS_ADC
  if (this_microphone->adc_) {
    config.mode = (i2s_mode_t) (config.mode | I2S_MODE_ADC_BUILT_IN);
    i2s_driver_install(this_microphone->parent_->get_port(), &config, 0, nullptr);

    i2s_set_adc_mode(ADC_UNIT_1, this_microphone->adc_channel_);
    i2s_adc_enable(this_microphone->parent_->get_port());
  } else
#endif
  {
    if (this_microphone->pdm_)
      config.mode = (i2s_mode_t) (config.mode | I2S_MODE_PDM);

    err = i2s_driver_install(this_microphone->parent_->get_port(), &config, 0, nullptr);
    if (err != ESP_OK) {
      event.type = TaskEventType::WARNING;
      event.err = err;
      xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

      event.type = TaskEventType::STOPPED;
      event.err = ESP_OK;
      xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

      while (true) {
        delay(10);
      }

      return;
    }
    
    i2s_pin_config_t pin_config = this_microphone->parent_->get_pin_config();
    pin_config.data_in_num = this_microphone->din_pin_;
    
    err = i2s_set_pin(this_microphone->parent_->get_port(), &pin_config);
    if (err != ESP_OK) {
      event.type = TaskEventType::WARNING;
      event.err = err;
      xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

      event.type = TaskEventType::STOPPED;
      event.err = ESP_OK;
      xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

      while (true) {
        delay(10);
      }

      return;
    }
  }

  event.type = TaskEventType::STARTED;
  xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);
  std::vector<int16_t> samples;

  while (true) {
    if (xQueueReceive(this_microphone->command_queue_, &command_event, 0) == pdTRUE) {
      if (command_event.stop) {
        // Stop signal from main thread
        break;
      }
    }
    size_t bytes_read;
    esp_err_t err =
        i2s_read(this_microphone->parent_->get_port(), buffer, BUFFER_SIZE, &bytes_read, (10 / portTICK_PERIOD_MS));
    if (err != ESP_OK) {
      event.type = TaskEventType::WARNING;
      event.err = err;
      xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);
    }
    
    if (bytes_read > 0) {
      // TODO: Handle 16 bits per sample, currently it won't allow that option at codegen stage

      // if (this_microphone->bits_per_sample_ == I2S_BITS_PER_SAMPLE_16BIT) {
      //   this_microphone->output_ring_buffer_->write(buffer, bytes_read);
      // } else if (this_microphone->bits_per_sample_ == I2S_BITS_PER_SAMPLE_32BIT) {

        size_t samples_read = bytes_read / sizeof(int32_t);
        samples.resize(samples_read);
        for (size_t i = 0; i < samples_read; i++) {
          int32_t temp = reinterpret_cast<int32_t *>(buffer)[i] >> 16;    // We are amplifying by a factor of 4 by only shifting 14 bits...
          samples[i] = clamp<int16_t>(temp, INT16_MIN, INT16_MAX);
        }
        size_t bytes_free = this_microphone->output_ring_buffer_->free();
        size_t bytes_to_write = samples_read * sizeof(int16_t);

        this_microphone->output_ring_buffer_->write((void *) samples.data(), samples_read * sizeof(int16_t));
      // }
    }

    event.type = TaskEventType::RUNNING;
    xQueueSend(this_microphone->event_queue_, &event, 0);
  }

  event.type = TaskEventType::STOPPING;
  xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

  allocator.deallocate(buffer, BUFFER_SIZE);
  i2s_stop(this_microphone->parent_->get_port());
  i2s_driver_uninstall(this_microphone->parent_->get_port());

  event.type = TaskEventType::STOPPED;
  xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void I2SAudioMicrophone::start() {
  if (this->is_failed())
    return;
  if (this->state_ == microphone::STATE_RUNNING)
    return;  // Already running
  this->state_ = microphone::STATE_STARTING;
}

void I2SAudioMicrophone::start_() {
  if (!this->parent_->try_lock()) {
    return;
  }

  xTaskCreate(I2SAudioMicrophone::read_task_, "microphone_task", 3584, (void *) this, 23, &this->read_task_handle_);
}

void I2SAudioMicrophone::stop() {
  if (this->state_ == microphone::STATE_STOPPED || this->is_failed())
    return;
  if (this->state_ == microphone::STATE_STARTING) {
    this->state_ = microphone::STATE_STOPPED;
    return;
  }
  this->state_ = microphone::STATE_STOPPING;
}

void I2SAudioMicrophone::stop_() {
  CommandEvent event;
  event.stop = true;
  xQueueSendToFront(this->command_queue_, &event, portMAX_DELAY);
}

size_t I2SAudioMicrophone::read(int16_t *buf, size_t len) {
  size_t bytes_read = this->output_ring_buffer_->read((void *) buf, len, 0);
  return bytes_read;
}

void I2SAudioMicrophone::read_() {
  std::vector<int16_t, ExternalRAMAllocator<int16_t>> samples;
  samples.resize(BUFFER_SIZE);
  // TODO this probably isn't correct
  size_t bytes_read = this->read(samples.data(), BUFFER_SIZE / sizeof(int16_t));
  samples.resize(bytes_read / sizeof(int16_t));
  // this->data_callbacks_.call(samples);
}

void I2SAudioMicrophone::loop() {
  this->watch_();
  switch (this->state_) {
    case microphone::STATE_STARTING:
      this->start_();
      break;
    case microphone::STATE_RUNNING:
      if (this->data_callbacks_.size() > 0) {
        this->read_();
      }
      break;
    case microphone::STATE_STOPPING:
      this->stop_();
      break;
    case microphone::STATE_STOPPED:
      break;
  }
}

void I2SAudioMicrophone::watch_() {
  TaskEvent event;
  while (xQueueReceive(this->event_queue_, &event, 0)) {
    switch (event.type) {
      case TaskEventType::STARTING:
        ESP_LOGD(TAG, "Starting I2S Audio Microphne");
        break;
      case TaskEventType::STARTED:
        ESP_LOGD(TAG, "Started I2S Audio Microphone");
        this->state_ = microphone::STATE_RUNNING;
        break;
      case TaskEventType::RUNNING:
        this->status_clear_warning();
        break;
      case TaskEventType::STOPPING:
        ESP_LOGD(TAG, "Stopping I2S Audio Microphone");
        break;
      case TaskEventType::STOPPED:
        this->state_ = microphone::STATE_STOPPED;

        vTaskDelete(this->read_task_handle_);
        this->read_task_handle_ = nullptr;
        this->parent_->unlock();

        this->output_ring_buffer_->reset();
        xQueueReset(this->event_queue_);
        xQueueReset(this->command_queue_);

        ESP_LOGD(TAG, "Stopped I2S Audio Microphone");
        break;
      case TaskEventType::WARNING:
        ESP_LOGW(TAG, "Error writing to I2S: %s", esp_err_to_name(event.err));
        this->status_set_warning();
        break;
      case TaskEventType::IDLE:
        break;
    }
  }
}

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32

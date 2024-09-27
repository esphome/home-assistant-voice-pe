#include "nabu_microphone.h"

#ifdef USE_ESP32

#include <driver/i2s.h>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/ring_buffer.h"

#ifdef USE_OTA
#include "esphome/components/ota/ota_backend.h"
#endif

namespace esphome {
namespace nabu_microphone {

static const size_t RING_BUFFER_LENGTH = 64;  // Measured in milliseconds
static const size_t QUEUE_LENGTH = 10;

static const size_t NUMBER_OF_CHANNELS = 2;
static const size_t DMA_BUFFER_SIZE = 160;
static const size_t DMA_BUFFERS_COUNT = 4;
static const size_t FRAMES_IN_ALL_DMA_BUFFERS = DMA_BUFFER_SIZE * DMA_BUFFERS_COUNT;
static const size_t SAMPLES_IN_ALL_DMA_BUFFERS = FRAMES_IN_ALL_DMA_BUFFERS * NUMBER_OF_CHANNELS;

static const size_t TASK_DELAY_MS = 10;

// TODO:
//   - Determine optimal buffer sizes (dma included)
//   - Determine appropriate timeout durations for FreeRTOS operations
//   - Test if stopping the microphone behaves properly

// Notes on things taken out/removed:
//   - Doesn't properly handle 16 bit samples
//   - Removed the watch_ function and handling any callbacks
//   - Channels are fixed to left and right for the XMOS chip

static const char *const TAG = "i2s_audio.microphone";

enum TaskNotificationBits : uint32_t {
  COMMAND_START = (1 << 0),  // Starts the main task purpose
  COMMAND_STOP = (1 << 1),   // stops the main task
};

void NabuMicrophoneChannel::setup() {
  const size_t ring_buffer_size = RING_BUFFER_LENGTH * this->parent_->get_sample_rate() / 1000 * sizeof(int16_t);
  this->ring_buffer_ = RingBuffer::create(ring_buffer_size);
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate ring buffer");
    this->mark_failed();
    return;
  }
}

void NabuMicrophoneChannel::loop() {
  if (this->parent_->is_running()) {
    if (this->is_muted_) {
      if (this->requested_stop_) {
        // The microphone was muted when stopping was requested
        this->state_ = microphone::STATE_STOPPED;
      } else {
        this->state_ = microphone::STATE_MUTED;
      }
    } else {
      this->state_ = microphone::STATE_RUNNING;
    }
  } else {
    this->state_ = microphone::STATE_STOPPED;
  }
}

void NabuMicrophone::setup() {
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

  this->event_queue_ = xQueueCreate(QUEUE_LENGTH, sizeof(TaskEvent));

#ifdef USE_OTA
  ota::get_global_ota_callback()->add_on_state_callback(
      [this](ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) {
        if (state == ota::OTA_STARTED) {
          if (this->read_task_handle_ != nullptr) {
            vTaskSuspend(this->read_task_handle_);
          }
        } else if (state == ota::OTA_ERROR) {
          if (this->read_task_handle_ != nullptr) {
            vTaskResume(this->read_task_handle_);
          }
        }
      });
#endif
}

void NabuMicrophone::mute() {
  if (this->channel_0_ != nullptr) {
    this->channel_0_->set_mute_state(true);
  }
  if (this->channel_1_ != nullptr) {
    this->channel_1_->set_mute_state(true);
  }
}

void NabuMicrophone::unmute() {
  if (this->channel_0_ != nullptr) {
    this->channel_0_->set_mute_state(false);
  }
  if (this->channel_1_ != nullptr) {
    this->channel_1_->set_mute_state(false);
  }
}

esp_err_t NabuMicrophone::start_i2s_driver_() {
  if (!this->parent_->try_lock()) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err;

  i2s_driver_config_t config = {
      .mode = (i2s_mode_t) (this->i2s_mode_ | I2S_MODE_RX),
      .sample_rate = this->sample_rate_,
      .bits_per_sample = this->bits_per_sample_,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = DMA_BUFFERS_COUNT,
      .dma_buf_len = DMA_BUFFER_SIZE,
      .use_apll = this->use_apll_,
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

#if SOC_I2S_SUPPORTS_ADC
  if (this->adc_) {
    config.mode = (i2s_mode_t) (config.mode | I2S_MODE_ADC_BUILT_IN);
    i2s_driver_install(this->parent_->get_port(), &config, 0, nullptr);

    i2s_set_adc_mode(ADC_UNIT_1, this->adc_channel_);
    i2s_adc_enable(this->parent_->get_port());
  } else
#endif
  {
    if (this->pdm_)
      config.mode = (i2s_mode_t) (config.mode | I2S_MODE_PDM);

    err = i2s_driver_install(this->parent_->get_port(), &config, 0, nullptr);
    if (err != ESP_OK) {
      return err;
    }

    i2s_pin_config_t pin_config = this->parent_->get_pin_config();
    pin_config.data_in_num = this->din_pin_;

    err = i2s_set_pin(this->parent_->get_port(), &pin_config);
    if (err != ESP_OK) {
      return err;
    }
  }

  return ESP_OK;
}

void NabuMicrophone::read_task_(void *params) {
  NabuMicrophone *this_microphone = (NabuMicrophone *) params;
  TaskEvent event;
  esp_err_t err;

  while (true) {
    uint32_t notification_bits = 0;
    xTaskNotifyWait(ULONG_MAX,           // clear all bits at start of wait
                    ULONG_MAX,           // clear all bits after waiting
                    &notification_bits,  // notifcation value after wait is finished
                    portMAX_DELAY);      // how long to wait

    if (notification_bits & TaskNotificationBits::COMMAND_START) {
      event.type = TaskEventType::STARTING;
      xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

      if ((this_microphone->channel_0_ != nullptr) && this_microphone->channel_0_->is_failed()) {
        event.type = TaskEventType::WARNING;
        event.err = ESP_ERR_INVALID_STATE;
        xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);
        continue;
      }

      if ((this_microphone->channel_1_ != nullptr) && this_microphone->channel_1_->is_failed()) {
        event.type = TaskEventType::WARNING;
        event.err = ESP_ERR_INVALID_STATE;
        xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);
        continue;
      }

      // Note, if we have 16 bit samples incoming, this requires modification
      ExternalRAMAllocator<int32_t> allocator(ExternalRAMAllocator<int32_t>::ALLOW_FAILURE);
      int32_t *buffer = allocator.allocate(SAMPLES_IN_ALL_DMA_BUFFERS);

      std::vector<int16_t, ExternalRAMAllocator<int16_t>> channel_0_samples;
      std::vector<int16_t, ExternalRAMAllocator<int16_t>> channel_1_samples;

      size_t channel_0_reserved_samples = 0;
      size_t channel_1_reserved_samples = 0;

      if (this_microphone->channel_0_ != nullptr) {
        channel_0_reserved_samples = FRAMES_IN_ALL_DMA_BUFFERS;
        channel_0_samples.reserve(channel_0_reserved_samples);
      }

      if (this_microphone->channel_1_ != nullptr) {
        channel_1_reserved_samples = FRAMES_IN_ALL_DMA_BUFFERS;
        channel_1_samples.reserve(channel_1_reserved_samples);
      }

      if ((buffer == nullptr) || (channel_0_samples.capacity() < channel_0_reserved_samples) ||
          (channel_1_samples.capacity() < channel_1_reserved_samples)) {
        event.type = TaskEventType::WARNING;
        event.err = ESP_ERR_NO_MEM;
        xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);
      } else {
        err = this_microphone->start_i2s_driver_();
        if (err != ESP_OK) {
          event.type = TaskEventType::WARNING;
          event.err = err;
          xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);
        } else {
          // TODO: Is this the ideal spot to reset the ring buffers?
          if (this_microphone->channel_0_ != nullptr)
            this_microphone->channel_0_->get_ring_buffer()->reset();
          if (this_microphone->channel_1_ != nullptr)
            this_microphone->channel_1_->get_ring_buffer()->reset();

          event.type = TaskEventType::STARTED;
          xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

          while (true) {
            notification_bits = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(0));
            if (notification_bits & TaskNotificationBits::COMMAND_STOP) {
              break;
            }

            size_t bytes_read;
            esp_err_t err =
                i2s_read(this_microphone->parent_->get_port(), buffer, SAMPLES_IN_ALL_DMA_BUFFERS * sizeof(int32_t),
                         &bytes_read, pdMS_TO_TICKS(TASK_DELAY_MS));
            if (err != ESP_OK) {
              event.type = TaskEventType::WARNING;
              event.err = err;
              xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);
            }

            if (bytes_read > 0) {
              // TODO: Handle 16 bits per sample, currently it won't allow that option at codegen stage

              const size_t samples_read = bytes_read / sizeof(int32_t);
              const size_t frames_read =
                  samples_read / NUMBER_OF_CHANNELS;  // Left and right channel samples combine into 1 frame

              uint8_t channel_0_shift = 16;
              if (this_microphone->channel_0_ != nullptr) {
                channel_0_shift -= this_microphone->channel_0_->get_amplify_shift();
              }
              uint8_t channel_1_shift = 16;
              if (this_microphone->channel_1_ != nullptr) {
                channel_1_shift -= this_microphone->channel_1_->get_amplify_shift();
              }

              for (size_t i = 0; i < frames_read; i++) {
                int32_t channel_0_sample = 0;
                if ((this_microphone->channel_0_ != nullptr) && (!this_microphone->channel_0_->get_mute_state())) {
                  channel_0_sample = buffer[NUMBER_OF_CHANNELS * i] >> channel_0_shift;
                  channel_0_samples[i] = (int16_t) clamp<int32_t>(channel_0_sample, INT16_MIN, INT16_MAX);
                }

                int32_t channel_1_sample = 0;
                if ((this_microphone->channel_1_ != nullptr) && (!this_microphone->channel_1_->get_mute_state())) {
                  channel_1_sample = buffer[NUMBER_OF_CHANNELS * i + 1] >> channel_1_shift;
                  channel_1_samples[i] = (int16_t) clamp<int32_t>(channel_1_sample, INT16_MIN, INT16_MAX);
                }
              }

              size_t bytes_to_write = frames_read * sizeof(int16_t);

              if (this_microphone->channel_0_ != nullptr) {
                this_microphone->channel_0_->get_ring_buffer()->write((void *) channel_0_samples.data(),
                                                                      bytes_to_write);
              }
              if (this_microphone->channel_1_ != nullptr) {
                this_microphone->channel_1_->get_ring_buffer()->write((void *) channel_1_samples.data(),
                                                                      bytes_to_write);
              }
            }

            event.type = TaskEventType::RUNNING;
            xQueueSend(this_microphone->event_queue_, &event, 0);
          }

          event.type = TaskEventType::STOPPING;
          xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);

          allocator.deallocate(buffer, SAMPLES_IN_ALL_DMA_BUFFERS);
          i2s_stop(this_microphone->parent_->get_port());
          i2s_driver_uninstall(this_microphone->parent_->get_port());

          this_microphone->parent_->unlock();

          event.type = TaskEventType::STOPPED;
          xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);
        }
      }
    }
    event.type = TaskEventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_microphone->event_queue_, &event, portMAX_DELAY);
  }
}

void NabuMicrophone::start() {
  if (this->is_failed())
    return;
  if ((this->state_ == microphone::STATE_STARTING) || (this->state_ == microphone::STATE_RUNNING))
    return;

  if (this->read_task_handle_ == nullptr) {
    xTaskCreate(NabuMicrophone::read_task_, "microphone_task", 3584, (void *) this, 23, &this->read_task_handle_);
  }

  // TODO: Should we overwrite? If stop and start are called in quick succession, what behavior do we want
  xTaskNotify(this->read_task_handle_, TaskNotificationBits::COMMAND_START, eSetValueWithoutOverwrite);
}

void NabuMicrophone::stop() {
  if (this->state_ == microphone::STATE_STOPPED || this->is_failed())
    return;

  xTaskNotify(this->read_task_handle_, TaskNotificationBits::COMMAND_STOP, eSetValueWithOverwrite);
}

void NabuMicrophone::loop() {
  if ((this->channel_0_ != nullptr) && (this->channel_0_->get_requested_stop()) && (this->channel_1_ != nullptr) &&
      (this->channel_1_->get_requested_stop())) {
    // Both microphone channels have requested a stop
    this->stop();
  }

  // Note this->state_ is only modified here based on the status of the task
  TaskEvent event;
  while (xQueueReceive(this->event_queue_, &event, 0)) {
    switch (event.type) {
      case TaskEventType::STARTING:
        this->state_ = microphone::STATE_STARTING;
        ESP_LOGD(TAG, "Starting I2S Audio Microphne");
        break;
      case TaskEventType::STARTED:
        this->state_ = microphone::STATE_RUNNING;
        ESP_LOGD(TAG, "Started I2S Audio Microphone");
        break;
      case TaskEventType::RUNNING:
        this->state_ = microphone::STATE_RUNNING;
        this->status_clear_warning();
        break;
      case TaskEventType::MUTED:
        this->state_ = microphone::STATE_MUTED;
        ESP_LOGD(TAG, "Muted I2S Audio Microphone");
        break;
      case TaskEventType::STOPPING:
        this->state_ = microphone::STATE_STOPPING;
        ESP_LOGD(TAG, "Stopping I2S Audio Microphone");
        break;
      case TaskEventType::STOPPED:
        this->state_ = microphone::STATE_STOPPED;
        ESP_LOGD(TAG, "Stopped I2S Audio Microphone");
        break;
      case TaskEventType::WARNING:
        ESP_LOGW(TAG, "Error involving I2S: %s", esp_err_to_name(event.err));
        this->status_set_warning();
        break;
      case TaskEventType::IDLE:
        break;
    }
  }
}

}  // namespace nabu_microphone
}  // namespace esphome

#endif  // USE_ESP32

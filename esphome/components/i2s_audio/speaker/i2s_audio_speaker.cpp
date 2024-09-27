#include "i2s_audio_speaker.h"

#ifdef USE_ESP32

#include <driver/i2s.h>

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace i2s_audio {

// static const size_t BUFFER_COUNT = 20;

static const uint8_t NUMBER_OF_CHANNELS = 2;  // Hard-coded expectation of stereo (2 channel) audio
static const size_t DMA_BUFFER_SIZE = 512;
static const size_t SAMPLES_IN_ONE_DMA_BUFFER = DMA_BUFFER_SIZE * NUMBER_OF_CHANNELS;
static const size_t DMA_BUFFERS_COUNT = 4;
static const size_t SAMPLES_IN_ALL_DMA_BUFFERS = SAMPLES_IN_ONE_DMA_BUFFER * DMA_BUFFERS_COUNT;
static const size_t OUTPUT_BUFFER_SAMPLES = 8192;  // Audio samples - keep small for fast pausing
static const size_t QUEUE_LENGTH = 10;
static const size_t TASK_DELAY_MS = 10;

static const char *const TAG = "i2s_audio.speaker";

enum SpeakerTaskNotificationBits : uint32_t {
  COMMAND_START = (1 << 0),            // Starts the main task purpose
  COMMAND_STOP = (1 << 1),             // stops the main task
  COMMAND_STOP_GRACEFULLY = (1 << 2),  // Stops the task once all data has been written
  COMMAND_RELOAD_CLOCK = (1 << 3),
};

void I2SAudioSpeaker::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Speaker...");

  this->speaker_event_queue_ = xQueueCreate(QUEUE_LENGTH, sizeof(TaskEvent));
  if (this->speaker_event_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create event queue");
    this->mark_failed();
    return;
  }
}

template<typename a, typename b> const uint8_t *convert_data_format(const a *from, b *to, size_t &bytes, bool repeat) {
  if (sizeof(a) == sizeof(b) && !repeat) {
    return reinterpret_cast<const uint8_t *>(from);
  }
  const b *result = to;
  for (size_t i = 0; i < bytes; i += sizeof(a)) {
    b value = static_cast<b>(*from++) << (sizeof(b) - sizeof(a)) * 8;
    *to++ = value;
    if (repeat)
      *to++ = value;
  }
  bytes *= (sizeof(b) / sizeof(a)) * (repeat ? 2 : 1);  // NOLINT
  return reinterpret_cast<const uint8_t *>(result);
}

void I2SAudioSpeaker::start() {
  if (this->is_failed())
    return;
  if ((this->state_ == speaker::STATE_STARTING) || (this->state_ == speaker::STATE_RUNNING))
    return;

  if (this->speaker_task_handle_ == nullptr) {
    xTaskCreate(I2SAudioSpeaker::speaker_task, "speaker_task", 8192, (void *) this, 23, &this->speaker_task_handle_);
  }

  if (this->speaker_task_handle_ != nullptr) {
    xTaskNotify(this->speaker_task_handle_, SpeakerTaskNotificationBits::COMMAND_START, eSetValueWithoutOverwrite);
    this->task_created_ = true;
  } else {
    // ERROR SITUATION!
  }
}

esp_err_t I2SAudioSpeaker::start_i2s_driver_() {
  if (!this->parent_->try_lock()) {
    return ESP_ERR_INVALID_STATE;
  }

  i2s_driver_config_t config = {
      .mode = (i2s_mode_t) (this->i2s_mode_ | I2S_MODE_TX),
      .sample_rate = this->sample_rate_,
      .bits_per_sample = this->bits_per_sample_,
      .channel_format = this->channel_,
      .communication_format = this->i2s_comm_fmt_,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = DMA_BUFFERS_COUNT,
      .dma_buf_len = DMA_BUFFER_SIZE,
      .use_apll = this->use_apll_,
      .tx_desc_auto_clear = true,
      .fixed_mclk = I2S_PIN_NO_CHANGE,
      .mclk_multiple = I2S_MCLK_MULTIPLE_256,
      .bits_per_chan = this->bits_per_channel_,
      // #if SOC_I2S_SUPPORTS_TDM
      //       .chan_mask = (i2s_channel_t) (I2S_TDM_ACTIVE_CH0 | I2S_TDM_ACTIVE_CH1),
      //       .total_chan = 2,
      //       .left_align = false,
      //       .big_edin = false,
      //       .bit_order_msb = false,
      //       .skip_msk = false,
      // #endif
  };

  esp_err_t err = i2s_driver_install(this->parent_->get_port(), &config, 0, nullptr);
  if (err != ESP_OK) {
    return err;
  }

  i2s_set_clk(this->parent_->get_port(), this->sample_rate_, this->bits_per_sample_, I2S_CHANNEL_MONO);

  i2s_pin_config_t pin_config = this->parent_->get_pin_config();
  pin_config.data_out_num = this->dout_pin_;

  err = i2s_set_pin(this->parent_->get_port(), &pin_config);

  if (err != ESP_OK) {
    return err;
  }

  return ESP_OK;
}

void I2SAudioSpeaker::speaker_task(void *params) {
  {
    uint32_t notification_bits = 0;
    xTaskNotifyWait(ULONG_MAX,           // clear all bits at start of wait
                    ULONG_MAX,           // clear all bits after waiting
                    &notification_bits,  // notifcation value after wait is finished
                    portMAX_DELAY);      // how long to wait

    I2SAudioSpeaker *this_speaker = (I2SAudioSpeaker *) params;

    TaskEvent event;
    esp_err_t err = ESP_OK;

    if (notification_bits & SpeakerTaskNotificationBits::COMMAND_START) {
      event.type = TaskEventType::STARTING;
      xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

      ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
      int16_t *data_buffer = allocator.allocate(SAMPLES_IN_ALL_DMA_BUFFERS);

      if (this_speaker->audio_ring_buffer_ == nullptr)
        this_speaker->audio_ring_buffer_ = RingBuffer::create(OUTPUT_BUFFER_SAMPLES * sizeof(int16_t));

      if ((data_buffer == nullptr) || (this_speaker->audio_ring_buffer_ == nullptr)) {
        err = ESP_ERR_NO_MEM;

        event.type = TaskEventType::WARNING;
        event.err = err;
        xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
      }

      if (event.err == ESP_OK) {
        err = this_speaker->start_i2s_driver_();

        if (err != ESP_OK) {
          event.type = TaskEventType::WARNING;
          event.err = err;
          xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
        } else {
          event.type = TaskEventType::STARTED;
          xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

          bool stop_gracefully = false;
          uint32_t last_data_received_time = millis();

          while (true) {
            notification_bits = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(0));

            if (notification_bits & SpeakerTaskNotificationBits::COMMAND_STOP) {
              break;
            } else if (notification_bits & SpeakerTaskNotificationBits::COMMAND_STOP_GRACEFULLY) {
              stop_gracefully = true;
            }

            size_t bytes_read = 0;
            size_t bytes_to_read = sizeof(int16_t) * SAMPLES_IN_ALL_DMA_BUFFERS;
            bytes_read = this_speaker->audio_ring_buffer_->read((void *) data_buffer, bytes_to_read,
                                                                pdMS_TO_TICKS(TASK_DELAY_MS));

            if (bytes_read > 0) {
              last_data_received_time = millis();
              size_t bytes_written = 0;

              if (this_speaker->bits_per_sample_ == I2S_BITS_PER_SAMPLE_16BIT) {
                i2s_write(this_speaker->parent_->get_port(), data_buffer, bytes_read, &bytes_written, portMAX_DELAY);
              } else {
                i2s_write_expand(this_speaker->parent_->get_port(), data_buffer, bytes_read, I2S_BITS_PER_SAMPLE_16BIT,
                                 this_speaker->bits_per_sample_, &bytes_written, portMAX_DELAY);
              }

              if (bytes_written != bytes_read) {
                event.type = TaskEventType::WARNING;
                event.err = ESP_ERR_INVALID_SIZE;
                xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
              }

            } else {
              // No data received
              if (stop_gracefully) {
                break;
              }

              i2s_zero_dma_buffer(this_speaker->parent_->get_port());
            }
          }

          i2s_zero_dma_buffer(this_speaker->parent_->get_port());

          event.type = TaskEventType::STOPPING;
          xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

          i2s_stop(this_speaker->parent_->get_port());
          i2s_driver_uninstall(this_speaker->parent_->get_port());

          this_speaker->parent_->unlock();
        }
      }

      if (this_speaker->audio_ring_buffer_ != nullptr) {
        this_speaker->audio_ring_buffer_.reset();  // Deallocates the ring buffer stored in the unique_ptr
        this_speaker->audio_ring_buffer_ = nullptr;
      }

      if (data_buffer != nullptr) {
        allocator.deallocate(data_buffer, SAMPLES_IN_ALL_DMA_BUFFERS);
      }

      event.type = TaskEventType::STOPPED;
      xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
    }
  }
  while (true) {
    delay(10);
  }
}

// void I2SAudioSpeaker::player_task(void *params) {
//   I2SAudioSpeaker *this_speaker = (I2SAudioSpeaker *) params;

//   TaskEvent event;
//   event.type = TaskEventType::STARTING;
//   xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);

//   i2s_driver_config_t config = {
//       .mode = (i2s_mode_t) (this_speaker->i2s_mode_ | I2S_MODE_TX),
//       .sample_rate = this_speaker->sample_rate_,
//       .bits_per_sample = this_speaker->bits_per_sample_,
//       .channel_format = this_speaker->channel_,
//       .communication_format = this_speaker->i2s_comm_fmt_,
//       .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
//       .dma_buf_count = 8,
//       .dma_buf_len = 256,
//       .use_apll = this_speaker->use_apll_,
//       .tx_desc_auto_clear = true,
//       .fixed_mclk = 0,
//       .mclk_multiple = I2S_MCLK_MULTIPLE_256,
//       .bits_per_chan = this_speaker->bits_per_channel_,
//   };
// #if SOC_I2S_SUPPORTS_DAC
//   if (this_speaker->internal_dac_mode_ != I2S_DAC_CHANNEL_DISABLE) {
//     config.mode = (i2s_mode_t) (config.mode | I2S_MODE_DAC_BUILT_IN);
//   }
// #endif

//   esp_err_t err = i2s_driver_install(this_speaker->parent_->get_port(), &config, 0, nullptr);
//   if (err != ESP_OK) {
//     event.type = TaskEventType::WARNING;
//     event.err = err;
//     xQueueSend(this_speaker->event_queue_, &event, 0);
//     event.type = TaskEventType::STOPPED;
//     xQueueSend(this_speaker->event_queue_, &event, 0);
//     while (true) {
//       delay(10);
//     }
//   }

// #if SOC_I2S_SUPPORTS_DAC
//   if (this_speaker->internal_dac_mode_ == I2S_DAC_CHANNEL_DISABLE) {
// #endif
//     i2s_pin_config_t pin_config = this_speaker->parent_->get_pin_config();
//     pin_config.data_out_num = this_speaker->dout_pin_;

//     i2s_set_pin(this_speaker->parent_->get_port(), &pin_config);
// #if SOC_I2S_SUPPORTS_DAC
//   } else {
//     i2s_set_dac_mode(this_speaker->internal_dac_mode_);
//   }
// #endif

//   DataEvent data_event;

//   event.type = TaskEventType::STARTED;
//   xQueueSend(this_speaker->event_queue_, &event, portMAX_DELAY);

//   int32_t buffer[BUFFER_SIZE];

//   while (true) {
//     if (xQueueReceive(this_speaker->buffer_queue_, &data_event, this_speaker->timeout_ / portTICK_PERIOD_MS) !=
//         pdTRUE) {
//       break;  // End of audio from main thread
//     }
//     if (data_event.stop) {
//       // Stop signal from main thread
//       xQueueReset(this_speaker->buffer_queue_);  // Flush queue
//       break;
//     }

//     const uint8_t *data = data_event.data;
//     size_t remaining = data_event.len;
//     switch (this_speaker->bits_per_sample_) {
//       case I2S_BITS_PER_SAMPLE_8BIT:
//       case I2S_BITS_PER_SAMPLE_16BIT: {
//         data = convert_data_format(reinterpret_cast<const int16_t *>(data), reinterpret_cast<int16_t *>(buffer),
//                                    remaining, this_speaker->channel_ == I2S_CHANNEL_FMT_ALL_LEFT);
//         break;
//       }
//       case I2S_BITS_PER_SAMPLE_24BIT:
//       case I2S_BITS_PER_SAMPLE_32BIT: {
//         data = convert_data_format(reinterpret_cast<const int16_t *>(data), reinterpret_cast<int32_t *>(buffer),
//                                    remaining, this_speaker->channel_ == I2S_CHANNEL_FMT_ALL_LEFT);
//         break;
//       }
//     }

//     while (remaining != 0) {
//       size_t bytes_written;
//       esp_err_t err =
//           i2s_write(this_speaker->parent_->get_port(), data, remaining, &bytes_written, (32 / portTICK_PERIOD_MS));
//       if (err != ESP_OK) {
//         event = {.type = TaskEventType::WARNING, .err = err};
//         if (xQueueSend(this_speaker->event_queue_, &event, 10 / portTICK_PERIOD_MS) != pdTRUE) {
//           ESP_LOGW(TAG, "Failed to send WARNING event");
//         }
//         continue;
//       }
//       data += bytes_written;
//       remaining -= bytes_written;
//     }
//   }

//   event.type = TaskEventType::STOPPING;
//   if (xQueueSend(this_speaker->event_queue_, &event, 10 / portTICK_PERIOD_MS) != pdTRUE) {
//     ESP_LOGW(TAG, "Failed to send STOPPING event");
//   }

//   i2s_zero_dma_buffer(this_speaker->parent_->get_port());

//   i2s_driver_uninstall(this_speaker->parent_->get_port());

//   event.type = TaskEventType::STOPPED;
//   if (xQueueSend(this_speaker->event_queue_, &event, 10 / portTICK_PERIOD_MS) != pdTRUE) {
//     ESP_LOGW(TAG, "Failed to send STOPPED event");
//   }

//   while (true) {
//     delay(10);
//   }
// }

void I2SAudioSpeaker::stop() { this->stop_(false); }

void I2SAudioSpeaker::finish() { this->stop_(true); }

void I2SAudioSpeaker::stop_(bool wait_on_empty) {
  if (this->is_failed())
    return;
  if (this->state_ == speaker::STATE_STOPPED)
    return;

  if (wait_on_empty) {
    xTaskNotify(this->speaker_task_handle_, SpeakerTaskNotificationBits::COMMAND_STOP_GRACEFULLY,
                eSetValueWithOverwrite);
  } else {
    xTaskNotify(this->speaker_task_handle_, SpeakerTaskNotificationBits::COMMAND_STOP, eSetValueWithOverwrite);
  }
}

void I2SAudioSpeaker::watch_() {
  TaskEvent event;
  while (xQueueReceive(this->speaker_event_queue_, &event, 0)) {
    switch (event.type) {
      case TaskEventType::STARTING:
        ESP_LOGD(TAG, "Starting Speaker");
        this->state_ = speaker::STATE_STARTING;
        break;
      case TaskEventType::STARTED:
        ESP_LOGD(TAG, "Started Speaker");
        this->state_ = speaker::STATE_RUNNING;
        break;
      case TaskEventType::STOPPING:
        ESP_LOGD(TAG, "Stopping Speaker");
        this->state_ = speaker::STATE_STOPPING;
        break;
      case TaskEventType::STOPPED:
        ESP_LOGD(TAG, "Stopped Speaker");
        this->state_ = speaker::STATE_STOPPED;
        if (this->task_created_) {
          vTaskDelete(this->speaker_task_handle_);
          this->speaker_task_handle_ = nullptr;
          this->task_created_ = false;
        }
        break;
      case TaskEventType::WARNING:
        ESP_LOGW(TAG, "Error writing to I2S: %s", esp_err_to_name(event.err));
        this->status_set_warning();
        break;
    }
  }
}


void I2SAudioSpeaker::loop() {
  this->watch_();
  // switch (this->state_) {
  //   case speaker::STATE_STARTING:
  //     this->start_();
  //     [[fallthrough]];
  //   case speaker::STATE_RUNNING:
  //   case speaker::STATE_STOPPING:
  //     this->watch_();
  //     break;
  //   case speaker::STATE_STOPPED:
  //     break;
  // }
}

size_t I2SAudioSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  // size_t I2SAudioSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->is_failed()) {
    ESP_LOGE(TAG, "Cannot play audio, speaker failed to setup");
    return 0;
  }
  if (this->state_ != speaker::STATE_RUNNING && this->state_ != speaker::STATE_STARTING) {
    this->start();
    // TODO: Should we add a delay so that the ring buffer has time to allocate?
  }

  if (this->audio_ring_buffer_.get() != nullptr) {
    return this->audio_ring_buffer_->write_without_replacement((void *) data, length, ticks_to_wait);
  }

  return 0;
}

bool I2SAudioSpeaker::has_buffered_data() const {
  if (this->audio_ring_buffer_.get() != nullptr) {
    return this->audio_ring_buffer_->available() > 0;
  }
  return false;
}

}  // namespace i2s_audio
}  // namespace esphome

#endif  // USE_ESP32

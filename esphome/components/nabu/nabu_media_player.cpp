#ifdef USE_ESP_IDF

#include "nabu_media_player.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "esp_dsp.h"

namespace esphome {
namespace nabu {

// TODO:
//  - Cleanup AudioResampler code (remove or refactor the esp_dsp fir filter)
//  - Clean up process around playing back local media files
//    - Create a registry of media files in Python
//    - Add a yaml action to play a specific media file
//
//
// Framework:
//  - Media player that can handle two streams; one for media and one for announcements
//    - If played together, they are mixed with the announcement stream staying at full volume
//    - The media audio can be further ducked via the ``set_ducking_reduction`` function
//  - Each stream is handled by an ``AudioPipeline`` object with three parts/tasks
//    - ``AudioReader`` handles reading from an HTTP source or from a PROGMEM flash set at compile time
//    - ``AudioDecoder`` handles decoding the audio file. All formats are limited to two channels and 16 bits per sample
//      - FLAC
//      - WAV
//      - MP3 (based on the libhelix decoder - a random mp3 file may be incompatible)
//    - ``AudioResampler`` handles converting the sample rate to the configured output sample rate and converting mono
//      to stereo
//      - The quality is not good, and it is slow! Please send audio at the configured sample rate to avoid these issues
//    - Each task will always run once started, but they will not doing anything until they are needed
//    - FreeRTOS Event Groups make up the inter-task communication
//    - The ``AudioPipeline`` sets up an output ring buffer for the Reader and Decoder parts. The next part/task
//      automatically pulls from the previous ring buffer
//  - The streams are mixed together in the ``AudioMixer`` task
//    - Each stream has a corresponding input buffer that the ``AudioResampler`` feeds directly
//    - Pausing the media stream is done here
//    - Media stream ducking is done here
//    - The output ring buffer feeds the ``speaker_task`` directly. It is kept small intentionally to avoid latency when
//      pausing
//  - Audio output is handled by the ``speaker_task``. It configures the I2S bus and copies audio from the mixer's
//    output ring buffer to the DMA buffers
//  - Media player commands are received by the ``control`` function. The commands are added to the
//    ``media_control_command_queue_`` to be processed in the component's loop
//    - Starting a stream intializes the appropriate pipeline or stops it if it is already running
//    - Volume and mute commands are achieved by the ``mute``, ``unmute``, ``set_volume`` functions. They communicate
//      directly with the DAC over I2C.
//      - Volume commands are ignored if the media control queue is full to avoid crashing when the track wheel is spun
//      fast
//    - Pausing is sent to the ``AudioMixer`` task. It only effects the media stream.
//  - The components main loop performs housekeeping:
//    - It reads the media control queue and processes it directly
//    - It watches the state of speaker and mixer tasks
//    - It determines the overall state of the media player by considering the state of each pipeline
//      - announcement playback takes highest priority

static const size_t QUEUE_LENGTH = 20;

static const uint8_t NUMBER_OF_CHANNELS = 2;  // Hard-coded expectation of stereo (2 channel) audio
static const size_t DMA_BUFFER_SIZE = 512;
static const size_t SAMPLES_IN_ONE_DMA_BUFFER = DMA_BUFFER_SIZE * NUMBER_OF_CHANNELS;
static const size_t DMA_BUFFERS_COUNT = 4;
static const size_t SAMPLES_IN_ALL_DMA_BUFFERS = SAMPLES_IN_ONE_DMA_BUFFER * DMA_BUFFERS_COUNT;

static const UBaseType_t MEDIA_PIPELINE_TASK_PRIORITY = 1;
static const UBaseType_t ANNOUNCEMENT_PIPELINE_TASK_PRIORITY = 1;
static const UBaseType_t MIXER_TASK_PRIORITY = 10;
static const UBaseType_t SPEAKER_TASK_PRIORITY = 23;

static const size_t TASK_DELAY_MS = 10;

static const float FIRST_BOOT_DEFAULT_VOLUME = 0.5f;

enum SpeakerTaskNotificationBits : uint32_t {
  COMMAND_START = (1 << 0),  // Starts the main task purpose
  COMMAND_STOP = (1 << 1),   // stops the main task
};

#define STATS_TASK_PRIO 3
#define STATS_TICKS pdMS_TO_TICKS(5000)
#define ARRAY_SIZE_OFFSET 5  // Increase this if print_real_time_stats returns ESP_ERR_INVALID_SIZE
#define configRUN_TIME_COUNTER_TYPE uint32_t
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
static esp_err_t print_real_time_stats(TickType_t xTicksToWait) {
  TaskStatus_t *start_array = NULL, *end_array = NULL;
  UBaseType_t start_array_size, end_array_size;
  configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
  esp_err_t ret;

  // Allocate array to store current task states
  start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
  size_t size = start_array_size * sizeof(TaskStatus_t);
  start_array = static_cast<TaskStatus_t *>(malloc(size));
  if (start_array == NULL) {
    ret = ESP_ERR_NO_MEM;
    free(start_array);
    free(end_array);
    return ret;
  }
  // Get current task states
  start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
  if (start_array_size == 0) {
    ret = ESP_ERR_INVALID_SIZE;
    free(start_array);
    free(end_array);
    return ret;
  }

  vTaskDelay(xTicksToWait);

  // Allocate array to store tasks states post delay
  end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
  end_array = static_cast<TaskStatus_t *>(malloc(sizeof(TaskStatus_t) * end_array_size));
  if (end_array == NULL) {
    ret = ESP_ERR_NO_MEM;
    free(start_array);
    free(end_array);
    return ret;
  }
  // Get post delay task states
  end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
  if (end_array_size == 0) {
    ret = ESP_ERR_INVALID_SIZE;
    free(start_array);
    free(end_array);
    return ret;
  }

  // Calculate total_elapsed_time in units of run time stats clock period.
  uint32_t total_elapsed_time = (end_run_time - start_run_time);
  if (total_elapsed_time == 0) {
    ret = ESP_ERR_INVALID_STATE;
    free(start_array);
    free(end_array);
    return ret;
  }

  printf("| Task | Run Time | Percentage\n");
  // Match each task in start_array to those in the end_array
  for (int i = 0; i < start_array_size; i++) {
    int k = -1;
    for (int j = 0; j < end_array_size; j++) {
      if (start_array[i].xHandle == end_array[j].xHandle) {
        k = j;
        // Mark that task have been matched by overwriting their handles
        start_array[i].xHandle = NULL;
        end_array[j].xHandle = NULL;
        break;
      }
    }
    // Check if matching task found
    if (k >= 0) {
      uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
      uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
      printf("| %s | %" PRIu32 " | %" PRIu32 "%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
    }
  }

  // Print unmatched tasks
  for (int i = 0; i < start_array_size; i++) {
    if (start_array[i].xHandle != NULL) {
      printf("| %s | Deleted\n", start_array[i].pcTaskName);
    }
  }
  for (int i = 0; i < end_array_size; i++) {
    if (end_array[i].xHandle != NULL) {
      printf("| %s | Created\n", end_array[i].pcTaskName);
    }
  }
  ret = ESP_OK;

  // exit:  // Common return path
  free(start_array);
  free(end_array);
  return ret;
}

static void stats_task(void *arg) {
  // Print real time stats periodically
  while (1) {
    printf("\n\nGetting real time stats over %" PRIu32 " ticks\n", STATS_TICKS);
    esp_err_t err = print_real_time_stats(STATS_TICKS);
    if (err == ESP_OK) {
      printf("Real time stats obtained\n");
    } else {
      printf("Error getting real time stats\n");
      printf("Error: %s", esp_err_to_name(err));
    }
  }
}

static const char *const TAG = "nabu_media_player";

void NabuMediaPlayer::setup() {
  // xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, STATS_TASK_PRIO, NULL, tskNO_AFFINITY);

  state = media_player::MEDIA_PLAYER_STATE_IDLE;

  this->media_control_command_queue_ = xQueueCreate(QUEUE_LENGTH, sizeof(MediaCallCommand));
  this->speaker_event_queue_ = xQueueCreate(QUEUE_LENGTH, sizeof(TaskEvent));

  this->pref_ = global_preferences->make_preference<VolumeRestoreState>(this->get_object_id_hash());

  VolumeRestoreState volume_restore_state;
  if (this->pref_.load(&volume_restore_state)) {
    this->set_volume_(volume_restore_state.volume);
    this->set_mute_state_(volume_restore_state.is_muted);
  } else {
    this->set_volume_(FIRST_BOOT_DEFAULT_VOLUME);
    this->set_mute_state_(false);
  }

  ESP_LOGI(TAG, "Set up nabu media player");
}

esp_err_t NabuMediaPlayer::start_i2s_driver_() {
  if (!this->parent_->try_lock()) {
    return ESP_ERR_INVALID_STATE;
  }

  i2s_driver_config_t config = {
      .mode = (i2s_mode_t) (this->parent_->get_i2s_mode() | I2S_MODE_TX),
      .sample_rate = this->sample_rate_,
      .bits_per_sample = this->bits_per_sample_,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = DMA_BUFFERS_COUNT,
      .dma_buf_len = DMA_BUFFER_SIZE,
      .use_apll = false,
      .tx_desc_auto_clear = true,
      .fixed_mclk = I2S_PIN_NO_CHANGE,
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

  esp_err_t err = i2s_driver_install(this->parent_->get_port(), &config, 0, nullptr);
  if (err != ESP_OK) {
    return err;
  }

  i2s_pin_config_t pin_config = this->parent_->get_pin_config();
  pin_config.data_out_num = this->dout_pin_;

  err = i2s_set_pin(this->parent_->get_port(), &pin_config);

  if (err != ESP_OK) {
    return err;
  }

  return ESP_OK;
}

void NabuMediaPlayer::speaker_task(void *params) {
  NabuMediaPlayer *this_speaker = (NabuMediaPlayer *) params;

  TaskEvent event;
  esp_err_t err;

  while (true) {
    uint32_t notification_bits = 0;
    xTaskNotifyWait(ULONG_MAX,           // clear all bits at start of wait
                    ULONG_MAX,           // clear all bits after waiting
                    &notification_bits,  // notifcation value after wait is finished
                    portMAX_DELAY);      // how long to wait

    if (notification_bits & SpeakerTaskNotificationBits::COMMAND_START) {
      event.type = EventType::STARTING;
      xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

      ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
      int16_t *buffer = allocator.allocate(SAMPLES_IN_ALL_DMA_BUFFERS);

      if (buffer == nullptr) {
        event.type = EventType::WARNING;
        event.err = ESP_ERR_NO_MEM;
        xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
      } else {
        err = this_speaker->start_i2s_driver_();

        if (err != ESP_OK) {
          event.type = EventType::WARNING;
          event.err = err;
          xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
        } else {
          event.type = EventType::STARTED;
          xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

          while (true) {
            notification_bits = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(0));

            if (notification_bits & SpeakerTaskNotificationBits::COMMAND_STOP) {
              break;
            }

            size_t bytes_read = 0;
            size_t bytes_to_read = sizeof(int16_t) * SAMPLES_IN_ALL_DMA_BUFFERS;
            bytes_read =
                this_speaker->audio_mixer_->read((uint8_t *) buffer, bytes_to_read, pdMS_TO_TICKS(TASK_DELAY_MS));

            if (bytes_read > 0) {
              size_t bytes_written;

#ifdef USE_AUDIO_DAC
              if (this_speaker->audio_dac_ == nullptr)
#endif
              {  // Fallback to software volume control if an audio dac isn't available
                int16_t volume_scale_factor =
                    this_speaker->software_volume_scale_factor_;  // Atomic read, so thread safe
                if (volume_scale_factor < INT16_MAX) {
#if defined(USE_ESP32_VARIANT_ESP32S3) || defined(USE_ESP32_VARIANT_ESP32)
                  dsps_mulc_s16_ae32(buffer, buffer, bytes_read / sizeof(int16_t), volume_scale_factor, 1, 1);
#else
                  dsps_mulc_s16_ansi(buffer, buffer, bytes_read / sizeof(int16_t), volume_scale_factor, 1, 1);
#endif
                }
              }

              if (this_speaker->bits_per_sample_ == I2S_BITS_PER_SAMPLE_16BIT) {
                i2s_write(this_speaker->parent_->get_port(), buffer, bytes_read, &bytes_written, portMAX_DELAY);
              } else {
                i2s_write_expand(this_speaker->parent_->get_port(), buffer, bytes_read, I2S_BITS_PER_SAMPLE_16BIT,
                                 this_speaker->bits_per_sample_, &bytes_written, portMAX_DELAY);
              }

              if (bytes_written != bytes_read) {
                event.type = EventType::WARNING;
                event.err = ESP_ERR_INVALID_SIZE;
                xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
              } else {
                event.type = EventType::RUNNING;
                xQueueSend(this_speaker->speaker_event_queue_, &event, 0);
              }

            } else {
              i2s_zero_dma_buffer(this_speaker->parent_->get_port());

              event.type = EventType::IDLE;
              xQueueSend(this_speaker->speaker_event_queue_, &event, 0);
            }
          }

          i2s_zero_dma_buffer(this_speaker->parent_->get_port());

          event.type = EventType::STOPPING;
          xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

          allocator.deallocate(buffer, SAMPLES_IN_ALL_DMA_BUFFERS);
          i2s_stop(this_speaker->parent_->get_port());
          i2s_driver_uninstall(this_speaker->parent_->get_port());

          this_speaker->parent_->unlock();

          event.type = EventType::STOPPED;
          xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
        }
      }
    }
  }
}

esp_err_t NabuMediaPlayer::start_pipeline_(AudioPipelineType type, bool url) {
  esp_err_t err = ESP_OK;

  if (this->audio_mixer_ == nullptr) {
    this->audio_mixer_ = make_unique<AudioMixer>();
    err = this->audio_mixer_->start("mixer", MIXER_TASK_PRIORITY);
    if (err != ESP_OK) {
      return err;
    }
  }

  if (this->speaker_task_handle_ == nullptr) {
    xTaskCreate(NabuMediaPlayer::speaker_task, "speaker_task", 3072, (void *) this, SPEAKER_TASK_PRIORITY,
                &this->speaker_task_handle_);
    if (this->speaker_task_handle_ == nullptr) {
      return ESP_FAIL;
    }
  }

  xTaskNotify(this->speaker_task_handle_, SpeakerTaskNotificationBits::COMMAND_START, eSetValueWithoutOverwrite);

  if (type == AudioPipelineType::MEDIA) {
    if (this->media_pipeline_ == nullptr) {
      this->media_pipeline_ = make_unique<AudioPipeline>(this->audio_mixer_.get(), type);
    }

    if (url) {
      err = this->media_pipeline_->start(this->media_url_.value(), this->sample_rate_, "media",
                                         MEDIA_PIPELINE_TASK_PRIORITY);
    } else {
      err = this->media_pipeline_->start(this->media_file_.value(), this->sample_rate_, "media",
                                         MEDIA_PIPELINE_TASK_PRIORITY);
    }

    if (this->is_paused_) {
      CommandEvent command_event;
      command_event.command = CommandEventType::RESUME_MEDIA;
      this->audio_mixer_->send_command(&command_event);
    }
    this->is_paused_ = false;
  } else if (type == AudioPipelineType::ANNOUNCEMENT) {
    if (this->announcement_pipeline_ == nullptr) {
      this->announcement_pipeline_ = make_unique<AudioPipeline>(this->audio_mixer_.get(), type);
    }

    if (url) {
      err = this->announcement_pipeline_->start(this->announcement_url_.value(), this->sample_rate_, "ann",
                                                ANNOUNCEMENT_PIPELINE_TASK_PRIORITY);
    } else {
      err = this->announcement_pipeline_->start(this->announcement_file_.value(), this->sample_rate_, "ann",
                                                ANNOUNCEMENT_PIPELINE_TASK_PRIORITY);
    }
  }

  return err;
}

void NabuMediaPlayer::watch_media_commands_() {
  MediaCallCommand media_command;
  CommandEvent command_event;
  esp_err_t err = ESP_OK;

  if (xQueueReceive(this->media_control_command_queue_, &media_command, 0) == pdTRUE) {
    if (media_command.new_url.has_value() && media_command.new_url.value()) {
      if (media_command.announce.has_value() && media_command.announce.value()) {
        err = this->start_pipeline_(AudioPipelineType::ANNOUNCEMENT, true);
      } else {
        err = this->start_pipeline_(AudioPipelineType::MEDIA, true);
      }
    }

    if (media_command.new_file.has_value() && media_command.new_file.value()) {
      if (media_command.announce.has_value() && media_command.announce.value()) {
        err = this->start_pipeline_(AudioPipelineType::ANNOUNCEMENT, false);
      } else {
        err = this->start_pipeline_(AudioPipelineType::MEDIA, false);
      }
    }

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Error starting the audio pipeline: %s", esp_err_to_name(err));
      this->status_set_error();
    } else {
      this->status_clear_error();
    }

    if (media_command.volume.has_value()) {
      this->set_volume_(media_command.volume.value());
      this->set_mute_state_(false);
      this->publish_state();
    }

    if (media_command.command.has_value()) {
      switch (media_command.command.value()) {
        case media_player::MEDIA_PLAYER_COMMAND_PLAY:
          if (this->is_paused_) {
            command_event.command = CommandEventType::RESUME_MEDIA;
            this->audio_mixer_->send_command(&command_event);
          }
          this->is_paused_ = false;
          break;
        case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
          if (!this->is_paused_) {
            command_event.command = CommandEventType::PAUSE_MEDIA;
            this->audio_mixer_->send_command(&command_event);
          }
          this->is_paused_ = true;
          break;
        case media_player::MEDIA_PLAYER_COMMAND_STOP:
          command_event.command = CommandEventType::STOP;
          if (media_command.announce.has_value() && media_command.announce.value()) {
            this->announcement_pipeline_->stop();
          } else {
            this->media_pipeline_->stop();
          }
          break;
        case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
          if (this->is_paused_) {
            command_event.command = CommandEventType::RESUME_MEDIA;
            this->audio_mixer_->send_command(&command_event);
            this->is_paused_ = false;
          } else {
            command_event.command = CommandEventType::PAUSE_MEDIA;
            this->audio_mixer_->send_command(&command_event);
            this->is_paused_ = true;
          }
          break;
        case media_player::MEDIA_PLAYER_COMMAND_MUTE: {
          this->set_mute_state_(true);

          this->publish_state();
          break;
        }
        case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
          this->set_mute_state_(false);
          this->publish_state();
          break;
        case media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP:
          this->set_volume_(std::min(1.0f, this->volume + this->volume_increment_));
          this->publish_state();
          break;
        case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN:
          this->set_volume_(std::max(0.0f, this->volume - this->volume_increment_));
          this->publish_state();
          break;
        default:
          break;
      }
    }
  }
}

void NabuMediaPlayer::watch_speaker_() {
  TaskEvent event;
  while (xQueueReceive(this->speaker_event_queue_, &event, 0)) {
    switch (event.type) {
      case EventType::STARTING:
        ESP_LOGD(TAG, "Starting Media Player Speaker");
        break;
      case EventType::STARTED:
        ESP_LOGD(TAG, "Started Media Player Speaker");
        break;
      case EventType::IDLE:
        break;
      case EventType::RUNNING:
        break;
      case EventType::STOPPING:
        ESP_LOGD(TAG, "Stopping Media Player Speaker");
        break;
      case EventType::STOPPED:
        xQueueReset(this->speaker_event_queue_);

        ESP_LOGD(TAG, "Stopped Media Player Speaker");
        break;
      case EventType::WARNING:
        ESP_LOGW(TAG, "Error writing to I2S: %s", esp_err_to_name(event.err));
        this->status_set_warning();
        break;
    }
  }
}

void NabuMediaPlayer::watch_mixer_() {
  TaskEvent event;
  if (this->audio_mixer_ != nullptr) {
    while (this->audio_mixer_->read_event(&event))
      if (event.type == EventType::WARNING) {
        ESP_LOGD(TAG, "Mixer encountered an error: %s", esp_err_to_name(event.err));
        this->status_set_error();
      }
  }
}

void NabuMediaPlayer::loop() {
  this->watch_media_commands_();
  this->watch_mixer_();
  this->watch_speaker_();

  // Determine state of the media player
  media_player::MediaPlayerState old_state = this->state;

  if (this->announcement_pipeline_ != nullptr)
    this->announcement_pipeline_state_ = this->announcement_pipeline_->get_state();

  if (this->media_pipeline_ != nullptr)
    this->media_pipeline_state_ = this->media_pipeline_->get_state();

  if (this->media_pipeline_state_ == AudioPipelineState::ERROR_READING) {
    ESP_LOGE(TAG, "Media pipeline encountered an error reading the file.");
  } else if (this->media_pipeline_state_ == AudioPipelineState::ERROR_DECODING) {
    ESP_LOGE(TAG, "Media pipeline encountered an error decoding the file.");
  } else if (this->media_pipeline_state_ == AudioPipelineState::ERROR_RESAMPLING) {
    ESP_LOGE(TAG, "Media pipeline encountered an error resampling the file.");
  }

  if (this->announcement_pipeline_state_ == AudioPipelineState::ERROR_READING) {
    ESP_LOGE(TAG, "Announcement pipeline encountered an error reading the file.");
  } else if (this->announcement_pipeline_state_ == AudioPipelineState::ERROR_DECODING) {
    ESP_LOGE(TAG, "Announcement pipeline encountered an error decoding the file.");
  } else if (this->announcement_pipeline_state_ == AudioPipelineState::ERROR_RESAMPLING) {
    ESP_LOGE(TAG, "Announcement pipeline encountered an error resampling the file.");
  }

  if (this->announcement_pipeline_state_ != AudioPipelineState::STOPPED) {
    this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
  } else {
    if (this->media_pipeline_state_ == AudioPipelineState::STOPPED) {
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
    } else if (this->is_paused_) {
      this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
    } else {
      this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
    }
  }

  if (this->state != old_state) {
    this->publish_state();
  }
}

void NabuMediaPlayer::set_ducking_reduction(uint8_t decibel_reduction, float duration) {
  if (this->audio_mixer_ != nullptr) {
    CommandEvent command_event;
    command_event.command = CommandEventType::DUCK;
    command_event.decibel_reduction = decibel_reduction;

    // Convert the duration in seconds to number of samples, accounting for the sample rate and number of channels
    command_event.transition_samples = static_cast<size_t>(duration * this->sample_rate_ * NUMBER_OF_CHANNELS);
    this->audio_mixer_->send_command(&command_event);
  }
}

void NabuMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  MediaCallCommand media_command;

  if (call.get_announcement().has_value() && call.get_announcement().value()) {
    media_command.announce = true;
  } else {
    media_command.announce = false;
  }

  if (call.get_media_url().has_value()) {
    std::string new_uri = call.get_media_url().value();

    media_command.new_url = true;
    if (call.get_announcement().has_value() && call.get_announcement().value()) {
      this->announcement_url_ = new_uri;
    } else {
      this->media_url_ = new_uri;
    }
    xQueueSend(this->media_control_command_queue_, &media_command, portMAX_DELAY);
    return;
  }

  if (call.get_local_media_file().has_value()) {
    if (call.get_announcement().has_value() && call.get_announcement().value()) {
      this->announcement_file_ = call.get_local_media_file().value();
    } else {
      this->media_file_ = call.get_local_media_file().value();
    }
    media_command.new_file = true;
    xQueueSend(this->media_control_command_queue_, &media_command, portMAX_DELAY);
    return;
  }

  if (call.get_volume().has_value()) {
    media_command.volume = call.get_volume().value();
    // Wait 0 ticks for queue to be free, volume sets aren't that important!
    xQueueSend(this->media_control_command_queue_, &media_command, 0);
    return;
  }

  if (call.get_command().has_value()) {
    media_command.command = call.get_command().value();
    TickType_t ticks_to_wait = portMAX_DELAY;
    if ((call.get_command().value() == media_player::MEDIA_PLAYER_COMMAND_VOLUME_UP) ||
        (call.get_command().value() == media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN)) {
      ticks_to_wait = 0;  // Wait 0 ticks for queue to be free, volume sets aren't that important!
    }
    xQueueSend(this->media_control_command_queue_, &media_command, ticks_to_wait);
    return;
  }
}

media_player::MediaPlayerTraits NabuMediaPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.set_supports_pause(true);
  traits.get_supported_formats().push_back(
      media_player::MediaPlayerSupportedFormat{.format = "flac",
                                               .sample_rate = 48000,
                                               .num_channels = 2,
                                               .purpose = media_player::MediaPlayerFormatPurpose::PURPOSE_DEFAULT});
  traits.get_supported_formats().push_back(media_player::MediaPlayerSupportedFormat{
      .format = "flac",
      .sample_rate = 48000,
      .num_channels = 1,
      .purpose = media_player::MediaPlayerFormatPurpose::PURPOSE_ANNOUNCEMENT});
  return traits;
};

void NabuMediaPlayer::save_volume_restore_state_() {
  VolumeRestoreState volume_restore_state;
  volume_restore_state.volume = this->volume;
  volume_restore_state.is_muted = this->is_muted_;
  this->pref_.save(&volume_restore_state);
}

void NabuMediaPlayer::set_mute_state_(bool mute_state) {
#ifdef USE_AUDIO_DAC
  if (this->audio_dac_ != nullptr) {
    if (mute_state) {
      this->audio_dac_->set_mute_on();
    } else {
      this->audio_dac_->set_mute_off();
    }
  } else
#endif
  {  // Fall back to software mute control if there is no audio_dac or if it isn't configured
    if (mute_state) {
      this->software_volume_scale_factor_ = 0;
    } else {
      this->set_volume_(this->volume, false);  // restore previous volume
    }
  }

  this->is_muted_ = mute_state;

  this->save_volume_restore_state_();
}

void NabuMediaPlayer::set_volume_(float volume, bool publish) {
#ifdef USE_AUDIO_DAC
  if (this->audio_dac_ != nullptr) {
    this->audio_dac_->set_volume(volume);
  } else
#endif
  {  // Fall back to software volume control if there is no audio_dac or if it isn't configured
    // Use the decibel reduction table from audio_mixer.h for software volume control
    ssize_t decibel_index = remap<ssize_t, float>(volume, 1.0f, 0.0f, 0, decibel_reduction_table.size() - 1);
    this->software_volume_scale_factor_ = decibel_reduction_table[decibel_index];
  }

  if (publish) {
    this->volume = volume;
    this->save_volume_restore_state_();
  }
}

}  // namespace nabu
}  // namespace esphome
#endif

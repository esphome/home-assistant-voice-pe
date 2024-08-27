#ifdef USE_ESP_IDF

#include "nabu_media_player.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace esphome {
namespace nabu {

// TODO:
//  - Cleanup AudioResampler code (remove or refactor the esp_dsp fir filter)
//  - Idle muting can cut off parts of the audio. Replace commnented code with eventual XMOS command to cut power to
//    speaker amp
//  - Tune task memory requirements and potentially buffer sizes if issues appear
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

static const size_t QUEUE_COUNT = 20;

static const uint8_t NUMBER_OF_CHANNELS = 2;  // Hard-coded expectation of stereo (2 channel) audio
static const size_t DMA_BUFFER_COUNT = 4;
static const size_t DMA_BUFFER_SIZE = 512;
static const size_t BUFFER_SIZE = NUMBER_OF_CHANNELS * DMA_BUFFER_COUNT * DMA_BUFFER_SIZE;

static const UBaseType_t MEDIA_PIPELINE_TASK_PRIORITY = 1;
static const UBaseType_t ANNOUNCEMENT_PIPELINE_TASK_PRIORITY = 7;
static const UBaseType_t MIXER_TASK_PRIORITY = 10;
static const UBaseType_t SPEAKER_TASK_PRIORITY = 23;

static const size_t TASK_DELAY_MS = 5;

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

  this->media_control_command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(MediaCallCommand));

  this->speaker_event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));

  if (!this->get_dac_volume_().has_value() || !this->get_dac_mute_().has_value()) {
    ESP_LOGE(TAG, "Couldn't communicate with DAC");
    this->mark_failed();
    return;
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
      .dma_buf_count = DMA_BUFFER_COUNT,
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
      int16_t *buffer = allocator.allocate(BUFFER_SIZE);

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
            notification_bits = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(TASK_DELAY_MS));

            if (notification_bits & SpeakerTaskNotificationBits::COMMAND_STOP) {
              break;
            }

            size_t bytes_to_read = DMA_BUFFER_SIZE * sizeof(int16_t) * NUMBER_OF_CHANNELS;
            size_t bytes_read = 0;

            bytes_read = this_speaker->audio_mixer_->read((uint8_t *) buffer, bytes_to_read, 0);

            if (bytes_read > 0) {
              size_t bytes_written;
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
                xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
              }

            } else {
              i2s_zero_dma_buffer(this_speaker->parent_->get_port());

              event.type = EventType::IDLE;
              xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);
            }
          }

          i2s_zero_dma_buffer(this_speaker->parent_->get_port());

          event.type = EventType::STOPPING;
          xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

          allocator.deallocate(buffer, BUFFER_SIZE);
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
      this->unmute_();
      this->is_muted_ = false;
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
          this->mute_();
          this->is_muted_ = true;
          this->publish_state();
          break;
        }
        case media_player::MEDIA_PLAYER_COMMAND_UNMUTE:
          this->unmute_();
          this->is_muted_ = false;
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

  if (this->announcement_pipeline_state_ != AudioPipelineState::STOPPED) {
    this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
    if (this->is_idle_muted_ && !this->is_muted_) {
      // this->unmute_();
      this->is_idle_muted_ = false;
    }
  } else {
    if (this->media_pipeline_state_ == AudioPipelineState::STOPPED) {
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
      if (!this->is_idle_muted_) {
        // this->mute_();
        this->is_idle_muted_ = true;
      }
    } else if (this->is_paused_) {
      this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
      if (!this->is_idle_muted_) {
        // this->mute_();
        this->is_idle_muted_ = true;
      }
    } else {
      this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
      if (this->is_idle_muted_ && !this->is_muted_) {
        // this->unmute_();
        this->is_idle_muted_ = false;
      }
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

optional<float> NabuMediaPlayer::get_dac_volume_(bool publish) {
  if (!this->write_byte(DAC_PAGE_SELECTION_REGISTER, DAC_VOLUME_PAGE)) {
    ESP_LOGE(TAG, "Failed to switch to page 0 on DAC");
    return {};
  }

  uint8_t dac_volume = 0;
  if (!this->read_byte(DAC_LEFT_VOLUME_REGISTER, &dac_volume)) {
    ESP_LOGE(TAG, "Failed to read the volume from the DAC");
    return {};
  }

  float volume = remap<float, int8_t>(static_cast<int8_t>(dac_volume), -127, 48, 0.0f, 1.0f);
  if (publish) {
    this->volume = volume;
  }

  return volume;
}

bool NabuMediaPlayer::set_volume_(float volume, bool publish) {
  int8_t dac_volume = remap<int8_t, float>(volume, 0.0f, 1.0f, -127, 48);
  if (!this->write_byte(DAC_PAGE_SELECTION_REGISTER, DAC_VOLUME_PAGE)) {
    ESP_LOGE(TAG, "DAC failed to switch to volume page registers");
    return false;
  }

  if (!this->write_byte(DAC_LEFT_VOLUME_REGISTER, dac_volume) ||
      !this->write_byte(DAC_RIGHT_VOLUME_REGISTER, dac_volume)) {
    ESP_LOGE(TAG, "DAC failed to set volume for left and right channels");
    return false;
  }

  if (publish)
    this->volume = volume;

  return true;
}

optional<bool> NabuMediaPlayer::get_dac_mute_(bool publish) {
  if (!this->write_byte(DAC_PAGE_SELECTION_REGISTER, DAC_MUTE_PAGE)) {
    ESP_LOGE(TAG, "DAC failed to switch to mute page registers");
    return {};
  }

  uint8_t dac_mute_left = 0;
  uint8_t dac_mute_right = 0;
  if (!this->read_byte(DAC_LEFT_MUTE_REGISTER, &dac_mute_left) ||
      !this->read_byte(DAC_RIGHT_MUTE_REGISTER, &dac_mute_right)) {
    ESP_LOGE(TAG, "DAC failed to read mute status");
    return {};
  }

  bool is_muted = false;
  if (dac_mute_left == DAC_MUTE_COMMAND && dac_mute_right == DAC_MUTE_COMMAND) {
    is_muted = true;
  }

  if (publish) {
    this->is_muted_ = is_muted;
  }
  return is_muted;
}

bool NabuMediaPlayer::mute_() {
  if (!this->write_byte(DAC_PAGE_SELECTION_REGISTER, DAC_MUTE_PAGE)) {
    ESP_LOGE(TAG, "DAC failed to switch to mute page registers");
    return false;
  }

  if (!this->write_byte(DAC_LEFT_MUTE_REGISTER, DAC_MUTE_COMMAND) ||
      !(this->write_byte(DAC_RIGHT_MUTE_REGISTER, DAC_MUTE_COMMAND))) {
    ESP_LOGE(TAG, "DAC failed to mute left and right channels");
    return false;
  }

  return true;
}

bool NabuMediaPlayer::unmute_() {
  if (!this->write_byte(DAC_PAGE_SELECTION_REGISTER, DAC_MUTE_PAGE)) {
    ESP_LOGE(TAG, "DAC failed to switch to mute page registers");
    return false;
  }

  if (!this->write_byte(DAC_LEFT_MUTE_REGISTER, DAC_UNMUTE_COMMAND) ||
      !(this->write_byte(DAC_RIGHT_MUTE_REGISTER, DAC_UNMUTE_COMMAND))) {
    ESP_LOGE(TAG, "DAC failed to unmute left and right channels");
    return false;
  }

  return true;
}

#define AIC3204_PAGE_CTRL 0x00     // Register 0  - Page Control
#define AIC3204_SW_RST 0x01        // Register 1  - Software Reset
#define AIC3204_CLK_PLL1 0x04      // Register 4  - Clock Setting Register 1, Multiplexers
#define AIC3204_CLK_PLL2 0x05      // Register 5  - Clock Setting Register 2, P and R values
#define AIC3204_CLK_PLL3 0x06      // Register 6  - Clock Setting Register 3, J values
#define AIC3204_NDAC 0x0B          // Register 11 - NDAC Divider Value
#define AIC3204_MDAC 0x0C          // Register 12 - MDAC Divider Value
#define AIC3204_DOSR 0x0E          // Register 14 - DOSR Divider Value (LS Byte)
#define AIC3204_NADC 0x12          // Register 18 - NADC Divider Value
#define AIC3204_MADC 0x13          // Register 19 - MADC Divider Value
#define AIC3204_AOSR 0x14          // Register 20 - AOSR Divider Value
#define AIC3204_CODEC_IF 0x1B      // Register 27 - CODEC Interface Control
#define AIC3204_AUDIO_IF_5 0x20    // Register 32 - Audio Interface Setting Register 5
#define AIC3204_DAC_SIG_PROC 0x3C  // Register 60 - DAC Sig Processing Block Control
#define AIC3204_ADC_SIG_PROC 0x3D  // Register 61 - ADC Sig Processing Block Control
#define AIC3204_DAC_CH_SET1 0x3F   // Register 63 - DAC Channel Setup 1
#define AIC3204_DAC_CH_SET2 0x40   // Register 64 - DAC Channel Setup 2
#define AIC3204_DACL_VOL_D 0x41    // Register 65 - DAC Left Digital Vol Control
#define AIC3204_DACR_VOL_D 0x42    // Register 66 - DAC Right Digital Vol Control
#define AIC3204_DRC_ENABLE 0x44
#define AIC3204_ADC_CH_SET 0x51    // Register 81 - ADC Channel Setup
#define AIC3204_ADC_FGA_MUTE 0x52  // Register 82 - ADC Fine Gain Adjust/Mute

// Page 1
#define AIC3204_PWR_CFG 0x01       // Register 1  - Power Config
#define AIC3204_LDO_CTRL 0x02      // Register 2  - LDO Control
#define AIC3204_PLAY_CFG1 0x03     // Register 3  - Playback Config 1
#define AIC3204_PLAY_CFG2 0x04     // Register 4  - Playback Config 2
#define AIC3204_OP_PWR_CTRL 0x09   // Register 9  - Output Driver Power Control
#define AIC3204_CM_CTRL 0x0A       // Register 10 - Common Mode Control
#define AIC3204_HPL_ROUTE 0x0C     // Register 12 - HPL Routing Select
#define AIC3204_HPR_ROUTE 0x0D     // Register 13 - HPR Routing Select
#define AIC3204_HPL_GAIN 0x10      // Register 16 - HPL Driver Gain
#define AIC3204_HPR_GAIN 0x11      // Register 17 - HPR Driver Gain
#define AIC3204_HP_START 0x14      // Register 20 - Headphone Driver Startup
#define AIC3204_LPGA_P_ROUTE 0x34  // Register 52 - Left PGA Positive Input Route
#define AIC3204_LPGA_N_ROUTE 0x36  // Register 54 - Left PGA Negative Input Route
#define AIC3204_RPGA_P_ROUTE 0x37  // Register 55 - Right PGA Positive Input Route
#define AIC3204_RPGA_N_ROUTE 0x39  // Register 57 - Right PGA Negative Input Route
#define AIC3204_LPGA_VOL 0x3B      // Register 59 - Left PGA Volume
#define AIC3204_RPGA_VOL 0x3C      // Register 60 - Right PGA Volume
#define AIC3204_ADC_PTM 0x3D       // Register 61 - ADC Power Tune Config
#define AIC3204_AN_IN_CHRG 0x47    // Register 71 - Analog Input Quick Charging Config
#define AIC3204_REF_STARTUP 0x7B   // Register 123 - Reference Power Up Config

void NabuMediaPlayer::reconfigure_dac_new_settings() {
  // Set register page to 0
  this->write_byte(AIC3204_PAGE_CTRL, 0x00);

  // Initiate SW reset (PLL is powered off as part of reset)
  this->write_byte(AIC3204_SW_RST, 0x01);

  // Program clock settings

  // Default is CODEC_CLKIN is from MCLK pin. Don't need to change this.
  /*
  // Enable PLL, MCLK is input to PLL
  this->write_byte(AIC3204_CLK_PLL1, 0x03);
  // MCLK is 24.576MHz, R = 1, J = 4, D = 0, P = 3, PLL_CLK = MCLK * R * J.D / P
  this->write_byte(AIC3204_CLK_PLL2, 0xB1);
  this->write_byte(AIC3204_CLK_PLL3, 0x04);
  // Power up NDAC and set to 4, or could we disable PLL and just set NDAC to 3?
  this->write_byte(AIC3204_NDAC, 0x84);
  // Power up MDAC and set to 4
  this->write_byte(AIC3204_MDAC, 0x84);
  */
  // Power up NDAC and set to 2
  this->write_byte(AIC3204_NDAC, 0x82);

  /*** MODIFICATION HERE
   * MDAC*NDAC*FOSR*48Khz = mClk (which I believe is 24.576 MHz)
   * (See page 51 of
   * https://www.ti.com/lit/ml/slaa557/slaa557.pdf?ts=1723766936991&ref_url=https%253A%252F%252Fwww.ti.com%252F)
   * They didn't properly modify this when switching to the 48 kHz firmware
   * We do need MDAC*DOSR/32 >= the resource compute level for the processing block
   * So here 2*128/32 = 8, which is equal to processing block 1 's recourse compute
   * See https://www.ti.com/lit/an/slaa404c/slaa404c.pdf?ts=1723806329961 page 5 for workflow on how to determine
   * these settings
   */
  // Power up MDAC and set to 2 (was originally 6)
  this->write_byte(AIC3204_MDAC, 0x82);

  // // Power up NADC and set to 1
  // this->write_byte(AIC3204_NADC, 0x81);
  // // Power up MADC and set to 4
  // this->write_byte(AIC3204_MADC, 0x84);
  // Program DOSR = 128
  this->write_byte(AIC3204_DOSR, 0x80);
  // // Program AOSR = 128
  // this->write_byte(AIC3204_AOSR, 0x80);
  // // Set Audio Interface Config: I2S, 24 bits, slave mode, DOUT always driving.
  // this->write_byte(AIC3204_CODEC_IF, 0x20);
  // Set Audio Interface Config: I2S, 32 bits, slave mode, DOUT always driving.
  this->write_byte(AIC3204_CODEC_IF, 0x30);
  // For I2S Firmware only, set SCLK/MFP3 pin as Audio Data In
  this->write_byte(56, 0x02);
  this->write_byte(31, 0x01);
  this->write_byte(32, 0x01);
  // Program the DAC processing block to be used - PRB_P1
  this->write_byte(AIC3204_DAC_SIG_PROC, 0x01);
  // Program the ADC processing block to be used - PRB_R1
  this->write_byte(AIC3204_ADC_SIG_PROC, 0x01);
  // Select Page 1
  this->write_byte(AIC3204_PAGE_CTRL, 0x01);
  // Enable the internal AVDD_LDO:
  this->write_byte(AIC3204_LDO_CTRL, 0x09);

  //
  // Program Analog Blocks
  // ---------------------
  //
  // Disable Internal Crude AVdd in presence of external AVdd supply or before powering up internal AVdd LDO
  this->write_byte(AIC3204_PWR_CFG, 0x08);
  // Enable Master Analog Power Control
  this->write_byte(AIC3204_LDO_CTRL, 0x01);
  // Set Common Mode voltages: Full Chip CM to 0.9V and Output Common Mode for Headphone to 1.65V and HP powered from
  // LDOin @ 3.3V.

  /*** MODIFICATION HERE! ***
   * All page changes refer to the TLV320AIC3204 Application Reference Guide
   *
   * Page 125: Common mode control register, set d6 to 1 to make the full chip common mode = 0.75 v
   * We are using the internal AVdd regulator with a nominal output of 1.72 V (see LDO_CTRL_REGISTER on page 123)
   * Page 86 says to only set the common mode voltage to 0.9 v if AVdd >= 1.8... but it isn't
   * We do need to tweak the HPL and HPR gain settings further down, as page 47 says we have to compensate with a -2
   * gain
   */
  this->write_byte(AIC3204_CM_CTRL, 0b01000000);  // 0x33);

  // Set PowerTune Modes
  // Set the Left & Right DAC PowerTune mode to PTM_P3/4. Use Class-AB driver.
  this->write_byte(AIC3204_PLAY_CFG1, 0x00);
  this->write_byte(AIC3204_PLAY_CFG2, 0x00);
  // // Set the Left & Right DAC PowerTune mode to PTM_P3/4. Use Class-D driver.
  // this->write_byte(AIC3204_PLAY_CFG1, 0xC0);
  // this->write_byte(AIC3204_PLAY_CFG2, 0xC0);

  // // Set ADC PowerTune mode PTM_R4.
  // this->write_byte(AIC3204_ADC_PTM, 0x00);
  // // Set MicPGA startup delay to 3.1ms
  // this->write_byte(AIC3204_AN_IN_CHRG, 0x31);

  // Set the REF charging time to 40ms
  this->write_byte(AIC3204_REF_STARTUP, 0x01);
  // HP soft stepping settings for optimal pop performance at power up
  // Rpop used is 6k with N = 6 and soft step = 20usec. This should work with 47uF coupling
  // capacitor. Can try N=5,6 or 7 time constants as well. Trade-off delay vs “pop” sound.
  this->write_byte(AIC3204_HP_START, 0x25);
  // Route Left DAC to HPL
  this->write_byte(AIC3204_HPL_ROUTE, 0x08);
  // Route Right DAC to HPR
  this->write_byte(AIC3204_HPR_ROUTE, 0x08);
  // Route Left DAC to LOL
  this->write_byte(0x0e, 0x08);
  // Route Right DAC to LOR
  this->write_byte(0x0f, 0x08);
  // We are using Line input with low gain for PGA so can use 40k input R but lets stick to 20k for now.
  // // Route IN2_L to LEFT_P with 20K input impedance
  // this->write_byte(AIC3204_LPGA_P_ROUTE, 0x20);
  // // Route IN2_R to LEFT_M with 20K input impedance
  // this->write_byte(AIC3204_LPGA_N_ROUTE, 0x20);
  // // Route IN1_R to RIGHT_P with 20K input impedance
  // this->write_byte(AIC3204_RPGA_P_ROUTE, 0x80);
  // // Route IN1_L to RIGHT_M with 20K input impedance
  // this->write_byte(AIC3204_RPGA_N_ROUTE, 0x20);

  /**** MODIFICATION HERE
   *
   * We compensate for the gain from modifying the common voltage
   */

  // Unmute HPL and set gain to 0dB
  this->write_byte(AIC3204_HPL_GAIN,
                   0b00111110);  // -2dB gain per page 47 of TLV320AIC3204 Application Reference Guide //0x00);
  // Unmute HPR and set gain to 0dB
  this->write_byte(AIC3204_HPR_GAIN,
                   0b00111110);  // -2dB gain per page 47 of TLV320AIC3204 Application Reference Guide //0x00);

  // Unmute LOL and set gain to 0dB
  this->write_byte(0x12, 0x00);
  // Unmute LOR and set gain to 0dB
  this->write_byte(0x13, 0x00);
  // Unmute Left MICPGA, Set Gain to 0dB.
  this->write_byte(AIC3204_LPGA_VOL, 0x00);
  // Unmute Right MICPGA, Set Gain to 0dB.
  this->write_byte(AIC3204_RPGA_VOL, 0x00);
  // // Power up HPL and HPR drivers
  // this->write_byte(AIC3204_OP_PWR_CTRL, 0x30) == 0
  // Power up HPL and HPR, LOL and LOR drivers
  this->write_byte(AIC3204_OP_PWR_CTRL, 0x3C);

  delay(2500);

  //
  // Power Up DAC/ADC
  // ----------------
  //
  // Select Page 0
  this->write_byte(AIC3204_PAGE_CTRL, 0x00);
  // Disable DRC
  // this->write_byte(AIC3204_DRC_ENABLE, 0x0F);
  // Power up the Left and Right DAC Channels. Route Left data to Left DAC and Right data to Right DAC.
  // DAC Vol control soft step 1 step per DAC word clock.
  this->write_byte(AIC3204_DAC_CH_SET1, 0xd4);
  // Power up Left and Right ADC Channels, ADC vol ctrl soft step 1 step per ADC word clock.
  this->write_byte(AIC3204_ADC_CH_SET, 0xc0);
  // Unmute Left and Right DAC digital volume control
  this->write_byte(AIC3204_DAC_CH_SET2, 0x00);
  // Unmute Left and Right ADC Digital Volume Control.
  this->write_byte(AIC3204_ADC_FGA_MUTE, 0x00);
}
void NabuMediaPlayer::reconfigure_dac_old_settings() {
  // Set register page to 0
  this->write_byte(AIC3204_PAGE_CTRL, 0x00);

  // Initiate SW reset (PLL is powered off as part of reset)
  this->write_byte(AIC3204_SW_RST, 0x01);

  // Program clock settings

  // Default is CODEC_CLKIN is from MCLK pin. Don't need to change this.
  /*
  // Enable PLL, MCLK is input to PLL
  this->write_byte(AIC3204_CLK_PLL1, 0x03);
  // MCLK is 24.576MHz, R = 1, J = 4, D = 0, P = 3, PLL_CLK = MCLK * R * J.D / P
  this->write_byte(AIC3204_CLK_PLL2, 0xB1);
  this->write_byte(AIC3204_CLK_PLL3, 0x04);
  // Power up NDAC and set to 4, or could we disable PLL and just set NDAC to 3?
  this->write_byte(AIC3204_NDAC, 0x84);
  // Power up MDAC and set to 4
  this->write_byte(AIC3204_MDAC, 0x84);
  */
  // Power up NDAC and set to 2
  this->write_byte(AIC3204_NDAC, 0x82);
  // Power up MDAC and set to 6
  this->write_byte(AIC3204_MDAC, 0x86);
  // // Power up NADC and set to 1
  // this->write_byte(AIC3204_NADC, 0x81);
  // // Power up MADC and set to 4
  // this->write_byte(AIC3204_MADC, 0x84);
  // Program DOSR = 128
  this->write_byte(AIC3204_DOSR, 0x80);
  // // Program AOSR = 128
  // this->write_byte(AIC3204_AOSR, 0x80);
  // // Set Audio Interface Config: I2S, 24 bits, slave mode, DOUT always driving.
  // this->write_byte(AIC3204_CODEC_IF, 0x20);
  // Set Audio Interface Config: I2S, 32 bits, slave mode, DOUT always driving.
  this->write_byte(AIC3204_CODEC_IF, 0x30);
  // For I2S Firmware only, set SCLK/MFP3 pin as Audio Data In
  this->write_byte(56, 0x02);
  this->write_byte(31, 0x01);
  this->write_byte(32, 0x01);
  // Program the DAC processing block to be used - PRB_P1
  this->write_byte(AIC3204_DAC_SIG_PROC, 0x01);
  // Program the ADC processing block to be used - PRB_R1
  this->write_byte(AIC3204_ADC_SIG_PROC, 0x01);
  // Select Page 1
  this->write_byte(AIC3204_PAGE_CTRL, 0x01);
  // Enable the internal AVDD_LDO:
  this->write_byte(AIC3204_LDO_CTRL, 0x09);

  //
  // Program Analog Blocks
  // ---------------------
  //
  // Disable Internal Crude AVdd in presence of external AVdd supply or before powering up internal AVdd LDO
  this->write_byte(AIC3204_PWR_CFG, 0x08);
  // Enable Master Analog Power Control
  this->write_byte(AIC3204_LDO_CTRL, 0x01);
  // Set Common Mode voltages: Full Chip CM to 0.9V and Output Common Mode for Headphone to 1.65V and HP powered from
  // LDOin @ 3.3V.
  this->write_byte(AIC3204_CM_CTRL, 0x33);
  // Set PowerTune Modes
  // Set the Left & Right DAC PowerTune mode to PTM_P3/4. Use Class-AB driver.
  this->write_byte(AIC3204_PLAY_CFG1, 0x00);
  this->write_byte(AIC3204_PLAY_CFG2, 0x00);
  // // Set the Left & Right DAC PowerTune mode to PTM_P3/4. Use Class-D driver.
  // this->write_byte(AIC3204_PLAY_CFG1, 0xC0);
  // this->write_byte(AIC3204_PLAY_CFG2, 0xC0);

  // // Set ADC PowerTune mode PTM_R4.
  // this->write_byte(AIC3204_ADC_PTM, 0x00);
  // // Set MicPGA startup delay to 3.1ms
  // this->write_byte(AIC3204_AN_IN_CHRG, 0x31);

  // Set the REF charging time to 40ms
  this->write_byte(AIC3204_REF_STARTUP, 0x01);
  // HP soft stepping settings for optimal pop performance at power up
  // Rpop used is 6k with N = 6 and soft step = 20usec. This should work with 47uF coupling
  // capacitor. Can try N=5,6 or 7 time constants as well. Trade-off delay vs “pop” sound.
  this->write_byte(AIC3204_HP_START, 0x25);
  // Route Left DAC to HPL
  this->write_byte(AIC3204_HPL_ROUTE, 0x08);
  // Route Right DAC to HPR
  this->write_byte(AIC3204_HPR_ROUTE, 0x08);
  // Route Left DAC to LOL
  this->write_byte(0x0e, 0x08);
  // Route Right DAC to LOR
  this->write_byte(0x0f, 0x08);
  // We are using Line input with low gain for PGA so can use 40k input R but lets stick to 20k for now.
  // // Route IN2_L to LEFT_P with 20K input impedance
  // this->write_byte(AIC3204_LPGA_P_ROUTE, 0x20);
  // // Route IN2_R to LEFT_M with 20K input impedance
  // this->write_byte(AIC3204_LPGA_N_ROUTE, 0x20);
  // // Route IN1_R to RIGHT_P with 20K input impedance
  // this->write_byte(AIC3204_RPGA_P_ROUTE, 0x80);
  // // Route IN1_L to RIGHT_M with 20K input impedance
  // this->write_byte(AIC3204_RPGA_N_ROUTE, 0x20);
  // Unmute HPL and set gain to 0dB
  this->write_byte(AIC3204_HPL_GAIN, 0x00);
  // Unmute HPR and set gain to 0dB
  this->write_byte(AIC3204_HPR_GAIN, 0x00);
  // Unmute LOL and set gain to 0dB
  this->write_byte(0x12, 0x00);
  // Unmute LOR and set gain to 0dB
  this->write_byte(0x13, 0x00);
  // Unmute Left MICPGA, Set Gain to 0dB.
  this->write_byte(AIC3204_LPGA_VOL, 0x00);
  // Unmute Right MICPGA, Set Gain to 0dB.
  this->write_byte(AIC3204_RPGA_VOL, 0x00);
  // // Power up HPL and HPR drivers
  // this->write_byte(AIC3204_OP_PWR_CTRL, 0x30) == 0
  // Power up HPL and HPR, LOL and LOR drivers
  this->write_byte(AIC3204_OP_PWR_CTRL, 0x3C);

  delay(2500);

  //
  // Power Up DAC/ADC
  // ----------------
  //
  // Select Page 0
  this->write_byte(AIC3204_PAGE_CTRL, 0x00);
  // Disable DRC
  // this->write_byte(AIC3204_DRC_ENABLE, 0x0F);
  // Power up the Left and Right DAC Channels. Route Left data to Left DAC and Right data to Right DAC.
  // DAC Vol control soft step 1 step per DAC word clock.
  this->write_byte(AIC3204_DAC_CH_SET1, 0xd4);
  // Power up Left and Right ADC Channels, ADC vol ctrl soft step 1 step per ADC word clock.
  this->write_byte(AIC3204_ADC_CH_SET, 0xc0);
  // Unmute Left and Right DAC digital volume control
  this->write_byte(AIC3204_DAC_CH_SET2, 0x00);
  // Unmute Left and Right ADC Digital Volume Control.
  this->write_byte(AIC3204_ADC_FGA_MUTE, 0x00);
}

}  // namespace nabu
}  // namespace esphome
#endif

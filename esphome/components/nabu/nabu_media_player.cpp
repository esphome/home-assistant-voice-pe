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
//  - Tune task memory requirements and potentially buffer sizes if issues appear
//  - Biquad filters work for downsampling without handling float buffer carefully, upsampling will require some care
//  - Ducking improvements
//    - Ducking ratio probably isn't the best way to specify, as volume perception is not linear
//    - Add a YAML action for setting the ducking level instead of requiring a lambda
//  - Clean up process around playing back local media files
//    - Create a registry of media files in Python
//    - Add a yaml action to play a specific media file

static const size_t QUEUE_COUNT = 20;
static const size_t DMA_BUFFER_COUNT = 4;
static const size_t DMA_BUFFER_SIZE = 512;
static const size_t BUFFER_SIZE = DMA_BUFFER_COUNT * DMA_BUFFER_SIZE;

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
  // xSemaphoreTake(sync_stats_task, portMAX_DELAY);

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
    // vTaskDelay(STATS_TICKS);
  }
}

static const char *const TAG = "nabu_media_player";

void NabuMediaPlayer::setup() {
  // xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, STATS_TASK_PRIO, NULL, tskNO_AFFINITY);

  state = media_player::MEDIA_PLAYER_STATE_IDLE;

  this->media_control_command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(MediaCallCommand));

  this->speaker_command_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(CommandEvent));
  this->speaker_event_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(TaskEvent));

  this->audio_mixer_ = make_unique<AudioMixer>();
  this->audio_mixer_->start("mixer", 10);

  if (!this->parent_->try_lock()) {
    this->mark_failed();
    return;
  }

  xTaskCreate(NabuMediaPlayer::speaker_task, "speaker_task", 3072, (void *) this, 23, &this->speaker_task_handle_);

  this->get_dac_volume_();

  // if (!this->write_byte(DAC_PAGE_SELECTION_REGISTER, 0x01)) {
  //   ESP_LOGE(TAG, "DAC failed to switch register page");
  //   return;
  // }

  // if (!this->write_byte(0x7B, 0b00000001)) {  // 40 ms reference start up time on page 1, didn't help
  //                                             // if (!this->write_byte(0x40, 0b00010000)) { // auto mute...
  //   return;
  // }

  ESP_LOGI(TAG, "Set up nabu media player");
}

void NabuMediaPlayer::speaker_task(void *params) {
  NabuMediaPlayer *this_speaker = (NabuMediaPlayer *) params;

  TaskEvent event;
  CommandEvent command_event;

  event.type = EventType::STARTING;
  xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

  ExternalRAMAllocator<int16_t> allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);
  int16_t *buffer = allocator.allocate(2 * BUFFER_SIZE);

  if (buffer == nullptr) {
    event.type = EventType::WARNING;
    event.err = ESP_ERR_NO_MEM;
    xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  i2s_driver_config_t config = {
      .mode = (i2s_mode_t) (this_speaker->parent_->get_i2s_mode() | I2S_MODE_TX),
      .sample_rate = this_speaker->sample_rate_,
      .bits_per_sample = this_speaker->bits_per_sample_,
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

  esp_err_t err = i2s_driver_install(this_speaker->parent_->get_port(), &config, 0, nullptr);
  if (err != ESP_OK) {
    event.type = EventType::WARNING;
    event.err = err;
    xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  i2s_pin_config_t pin_config = this_speaker->parent_->get_pin_config();
  pin_config.data_out_num = this_speaker->dout_pin_;

  err = i2s_set_pin(this_speaker->parent_->get_port(), &pin_config);

  if (err != ESP_OK) {
    event.type = EventType::WARNING;
    event.err = err;
    xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

    event.type = EventType::STOPPED;
    event.err = ESP_OK;
    xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

    while (true) {
      delay(10);
    }

    return;
  }

  event.type = EventType::STARTED;
  xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

  while (true) {
    if (xQueueReceive(this_speaker->speaker_command_queue_, &command_event, (5 / portTICK_PERIOD_MS)) == pdTRUE) {
      if (command_event.command == CommandEventType::STOP) {
        // Stop signal from main thread
        break;
      }
    }

    size_t delay_ms = 10;
    size_t bytes_to_read = DMA_BUFFER_SIZE * sizeof(int16_t) * 2;  // *2 for stereo
    // if (event.type != TaskEventType::RUNNING) {
    //   // Fill the entire DMA buffer if there is audio being outputed
    //   bytes_to_read = DMA_BUFFER_COUNT*DMA_BUFFER_SIZE*sizeof(int16_t);
    // }

    size_t bytes_read = 0;

    bytes_read = this_speaker->audio_mixer_->read((uint8_t *) buffer, bytes_to_read, (delay_ms / portTICK_PERIOD_MS));

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
        event.err = ESP_ERR_TIMEOUT;  // TODO: not the correct error...
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

  event.type = EventType::STOPPED;
  xQueueSend(this_speaker->speaker_event_queue_, &event, portMAX_DELAY);

  while (true) {
    delay(10);
  }
}

void NabuMediaPlayer::watch_media_commands_() {
  MediaCallCommand media_command;
  CommandEvent command_event;

  if (xQueueReceive(this->media_control_command_queue_, &media_command, 0) == pdTRUE) {
    if (media_command.new_url.has_value() && media_command.new_url.value()) {
      if (media_command.announce.has_value() && media_command.announce.value()) {
        if (this->announcement_pipeline_ == nullptr) {
          this->announcement_pipeline_ =
              make_unique<AudioPipeline>(this->audio_mixer_.get(), AudioPipelineType::ANNOUNCEMENT);
        }

        this->announcement_pipeline_->start(this->announcement_url_.value(), this->sample_rate_, "ann", 7);
      } else {
        if (this->media_pipeline_ == nullptr) {
          this->media_pipeline_ = make_unique<AudioPipeline>(this->audio_mixer_.get(), AudioPipelineType::MEDIA);
        }

        this->media_pipeline_->start(this->media_url_.value(), this->sample_rate_, "media", 2);

        if (this->is_paused_) {
          CommandEvent command_event;
          command_event.command = CommandEventType::RESUME_MEDIA;
          this->audio_mixer_->send_command(&command_event);
        }
        this->is_paused_ = false;
      }
    }

    if (media_command.new_file.has_value() && media_command.new_file.value()) {
      if (media_command.announce.has_value() && media_command.announce.value()) {
        if (this->announcement_pipeline_ == nullptr) {
          this->announcement_pipeline_ =
              make_unique<AudioPipeline>(this->audio_mixer_.get(), AudioPipelineType::ANNOUNCEMENT);
        }

        this->announcement_pipeline_->start(this->announcement_file_.value(), this->sample_rate_, "ann", 7);
      } else {
        if (this->media_pipeline_ == nullptr) {
          this->media_pipeline_ = make_unique<AudioPipeline>(this->audio_mixer_.get(), AudioPipelineType::MEDIA);
        }

        this->media_pipeline_->start(this->media_file_.value(), this->sample_rate_, "media", 5);

        if (this->is_paused_) {
          CommandEvent command_event;
          command_event.command = CommandEventType::RESUME_MEDIA;
          this->audio_mixer_->send_command(&command_event);
        }
        this->is_paused_ = false;
      }
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
          if (this->media_pipeline_state_ == AudioPipelineState::PLAYING) {
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
            this->is_paused_ = false;
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
          this->set_volume_(std::min(1.0f, this->volume + 0.05f));
          this->publish_state();
          break;
        case media_player::MEDIA_PLAYER_COMMAND_VOLUME_DOWN:
          this->set_volume_(std::max(0.0f, this->volume - 0.05f));
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
        vTaskDelete(this->speaker_task_handle_);
        this->speaker_task_handle_ = nullptr;
        this->parent_->unlock();

        xQueueReset(this->speaker_event_queue_);
        xQueueReset(this->speaker_command_queue_);

        ESP_LOGD(TAG, "Stopped Media Player Speaker");
        break;
      case EventType::WARNING:
        ESP_LOGW(TAG, "Error writing to I2S: %s", esp_err_to_name(event.err));
        this->status_set_warning();
        break;
    }
  }
}

void NabuMediaPlayer::watch_() {
  TaskEvent event;
  if (this->audio_mixer_ != nullptr) {
    while (this->audio_mixer_->read_event(&event))
      ;
  }
}

void NabuMediaPlayer::loop() {
  this->watch_media_commands_();
  this->watch_();
  this->watch_speaker_();

  // Determine state of the media player
  media_player::MediaPlayerState old_state = this->state;

  if (this->announcement_pipeline_ != nullptr)
    this->announcement_pipeline_state_ = this->announcement_pipeline_->get_state();

  if (this->media_pipeline_ != nullptr)
    this->media_pipeline_state_ = this->media_pipeline_->get_state();

  if ((this->announcement_pipeline_state_ != AudioPipelineState::STOPPING) &&
      (this->announcement_pipeline_state_ != AudioPipelineState::STOPPED)) {
    this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
    if (this->is_idle_muted_ && !this->is_muted_) {
      // this->unmute_();
      this->is_idle_muted_ = false;
      ESP_LOGD(TAG, "Unmuting as output is not idle");
    }
  } else {
    if (this->is_paused_) {
      this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
      if (!this->is_idle_muted_) {
        // this->mute_();
        this->is_idle_muted_ = true;
      }
    } else if ((this->media_pipeline_state_ == AudioPipelineState::STOPPING) ||
               (this->media_pipeline_state_ == AudioPipelineState::STOPPED)) {
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
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

void NabuMediaPlayer::set_ducking_ratio(float ducking_ratio) {
  if (this->audio_mixer_ != nullptr) {
    CommandEvent command_event;
    command_event.command = CommandEventType::DUCK;
    command_event.ducking_ratio = ducking_ratio;
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

}  // namespace nabu
}  // namespace esphome
#endif

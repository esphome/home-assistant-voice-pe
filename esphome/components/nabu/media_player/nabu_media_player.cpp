#include "nabu_media_player.h"

#ifdef USE_ESP_IDF

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace esphome {
namespace nabu {

// #define STATS_TASK_PRIO 3
// #define STATS_TICKS pdMS_TO_TICKS(2000)
// #define ARRAY_SIZE_OFFSET 5  // Increase this if print_real_time_stats returns ESP_ERR_INVALID_SIZE
// #define configRUN_TIME_COUNTER_TYPE uint32_t
// #define CONFIG_FREERTOS_NUMBER_OF_CORES 2
// static esp_err_t print_real_time_stats(TickType_t xTicksToWait) {
//   TaskStatus_t *start_array = NULL, *end_array = NULL;
//   UBaseType_t start_array_size, end_array_size;
//   configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
//   esp_err_t ret;

//   // Allocate array to store current task states
//   start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
//   size_t size = start_array_size * sizeof(TaskStatus_t);
//   start_array = static_cast<TaskStatus_t *>(malloc(size));
//   if (start_array == NULL) {
//     ret = ESP_ERR_NO_MEM;
//     free(start_array);
//     free(end_array);
//     return ret;
//   }
//   // Get current task states
//   start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
//   if (start_array_size == 0) {
//     ret = ESP_ERR_INVALID_SIZE;
//     free(start_array);
//     free(end_array);
//     return ret;
//   }

//   vTaskDelay(xTicksToWait);

//   // Allocate array to store tasks states post delay
//   end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
//   end_array = static_cast<TaskStatus_t *>(malloc(sizeof(TaskStatus_t) * end_array_size));
//   if (end_array == NULL) {
//     ret = ESP_ERR_NO_MEM;
//     free(start_array);
//     free(end_array);
//     return ret;
//   }
//   // Get post delay task states
//   end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
//   if (end_array_size == 0) {
//     ret = ESP_ERR_INVALID_SIZE;
//     free(start_array);
//     free(end_array);
//     return ret;
//   }

//   // Calculate total_elapsed_time in units of run time stats clock period.
//   uint32_t total_elapsed_time = (end_run_time - start_run_time);
//   if (total_elapsed_time == 0) {
//     ret = ESP_ERR_INVALID_STATE;
//     free(start_array);
//     free(end_array);
//     return ret;
//   }

//   printf("| Task | Run Time | Percentage\n");
//   // Match each task in start_array to those in the end_array
//   for (int i = 0; i < start_array_size; i++) {
//     int k = -1;
//     for (int j = 0; j < end_array_size; j++) {
//       if (start_array[i].xHandle == end_array[j].xHandle) {
//         k = j;
//         // Mark that task have been matched by overwriting their handles
//         start_array[i].xHandle = NULL;
//         end_array[j].xHandle = NULL;
//         break;
//       }
//     }
//     // Check if matching task found
//     if (k >= 0) {
//       uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
//       uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time *
//       CONFIG_FREERTOS_NUMBER_OF_CORES); printf("| %s | %" PRIu32 " | %" PRIu32 "%%\n", start_array[i].pcTaskName,
//       task_elapsed_time, percentage_time);
//     }
//   }

//   // Print unmatched tasks
//   for (int i = 0; i < start_array_size; i++) {
//     if (start_array[i].xHandle != NULL) {
//       printf("| %s | Deleted\n", start_array[i].pcTaskName);
//     }
//   }
//   for (int i = 0; i < end_array_size; i++) {
//     if (end_array[i].xHandle != NULL) {
//       printf("| %s | Created\n", end_array[i].pcTaskName);
//     }
//   }
//   ret = ESP_OK;

//   // exit:  // Common return path
//   free(start_array);
//   free(end_array);
//   return ret;
// }

// static void stats_task(void *arg) {
//   // xSemaphoreTake(sync_stats_task, portMAX_DELAY);

//   // Print real time stats periodically
//   while (1) {
//     printf("\n\nGetting real time stats over %" PRIu32 " ticks\n", STATS_TICKS);
//     esp_err_t err = print_real_time_stats(STATS_TICKS);
//     if (err == ESP_OK) {
//       printf("Real time stats obtained\n");
//     } else {
//       printf("Error getting real time stats\n");
//       printf("Error: %s", esp_err_to_name(err));
//     }
//     vTaskDelay(STATS_TICKS);
//   }
// }

// Major TODOs:
//  - Handle reading wav headers... the function exists (but needs to be more robust) -> avoids pops at start of
//  playback
//  - MP3 playback support
//  - Handle stereo streams
//    - Careful if media stream and announcement stream have different number of channels
//    - Eventually I want the speaker component to accept stereo audio by default
//    - PCM streams should send the essential details to each step in this process to automatically handle different
//      streaming characteristics
//  - Implement a resampler... we probably won't be able to change on-the-fly the sample rate before feeding into the
//    XMOS chip
//    - only 16 bit mono channel 16 kHz audio is supported at the momemnt!
//  - Buffer sizes/task memory usage is not optimized... at all! These need to be tuned...

static const char *const TAG = "nabu_media_player";
// static const size_t TRANSFER_BUFFER_SIZE = 8192;

void NabuMediaPlayer::setup() {
  // xTaskCreatePinnedToCore(stats_task, "stats", 4096, NULL, STATS_TASK_PRIO, NULL, tskNO_AFFINITY);

  state = media_player::MEDIA_PLAYER_STATE_IDLE;

  this->media_control_command_queue_ = xQueueCreate(1, sizeof(MediaCallCommand));

  ESP_LOGI(TAG, "Set up nabu media player");
}

void NabuMediaPlayer::watch_media_commands_() {
  MediaCallCommand media_command;
  CommandEvent command_event;

  if (xQueueReceive(this->media_control_command_queue_, &media_command, 0) == pdTRUE) {
    if (media_command.new_url.has_value() && media_command.new_url.value()) {
      if (this->combine_streamer_ == nullptr) {
        {
          this->combine_streamer_ = make_unique<CombineStreamer>();
          this->speaker_->set_combine_streamer(this->combine_streamer_.get());
        }
      }
      this->combine_streamer_->start("mixer");

      if (media_command.announce.has_value() && media_command.announce.value()) {
        if (this->announcement_pipeline_ == nullptr) {
          this->announcement_pipeline_ =
              make_unique<Pipeline>(this->combine_streamer_.get(), PipelineType::ANNOUNCEMENT);
        }
        this->announcement_pipeline_->start(this->announcement_url_.value(), "ann_pipe");
        // }
      } else {
        if (this->media_pipeline_ == nullptr) {
          this->media_pipeline_ = make_unique<Pipeline>(this->combine_streamer_.get(), PipelineType::MEDIA);
        }
        this->media_pipeline_->start(this->media_url_.value(), "med_pipe");
        // }
        if (this->is_paused_) {
          command_event.command = CommandEventType::RESUME_MEDIA;
          this->combine_streamer_->send_command(&command_event);
        }
        this->is_paused_ = false;
      }
    }

    if (media_command.command.has_value()) {
      switch (media_command.command.value()) {
        case media_player::MEDIA_PLAYER_COMMAND_PLAY:
          if (this->is_paused_) {
            command_event.command = CommandEventType::RESUME_MEDIA;
            this->combine_streamer_->send_command(&command_event);
          }
          this->is_paused_ = false;
          break;
        case media_player::MEDIA_PLAYER_COMMAND_PAUSE:
          if (this->media_pipeline_state_ == PipelineState::PLAYING) {
            command_event.command = CommandEventType::PAUSE_MEDIA;
            this->combine_streamer_->send_command(&command_event);
          }
          this->is_paused_ = true;
          break;
        case media_player::MEDIA_PLAYER_COMMAND_STOP:
          command_event.command = CommandEventType::STOP;
          this->media_pipeline_->send_command(&command_event);
          this->is_paused_ = false;
          break;
        case media_player::MEDIA_PLAYER_COMMAND_TOGGLE:
          if (this->is_paused_) {
            command_event.command = CommandEventType::RESUME_MEDIA;
            this->combine_streamer_->send_command(&command_event);
            this->is_paused_ = false;
          } else {
            command_event.command = CommandEventType::PAUSE_MEDIA;
            this->combine_streamer_->send_command(&command_event);
            this->is_paused_ = true;
          }
          break;
        default:
          break;
      }
    }
  }
}
// TODO: Reduce code redundancy
void NabuMediaPlayer::watch_() {
  TaskEvent event;

  if (this->announcement_pipeline_ != nullptr) {
    while (this->announcement_pipeline_->read_event(&event)) {
      switch (event.type) {
        case EventType::STARTING:
          this->announcement_pipeline_state_ = PipelineState::STARTING;
          ESP_LOGD(TAG, "Starting Announcement Playback");
          break;
        case EventType::STARTED:
          ESP_LOGD(TAG, "Started Announcement Playback");
          this->announcement_pipeline_state_ = PipelineState::STARTED;
          break;
        case EventType::IDLE:
          this->announcement_pipeline_state_ = PipelineState::PLAYING;
          break;
        case EventType::RUNNING:
          this->announcement_pipeline_state_ = PipelineState::PLAYING;
          this->status_clear_warning();
          break;
        case EventType::STOPPING:
          ESP_LOGD(TAG, "Stopping Announcement Playback");
          this->announcement_pipeline_state_ = PipelineState::STOPPING;
          break;
        case EventType::STOPPED: {
          this->announcement_pipeline_->stop();
          ESP_LOGD(TAG, "Stopped Announcement Playback");
          this->announcement_pipeline_state_ = PipelineState::STOPPED;
          break;
        }
        case EventType::WARNING:
          ESP_LOGW(TAG, "Error reading announcement: %s", esp_err_to_name(event.err));
          this->status_set_warning(esp_err_to_name(event.err));
          break;
      }
    }
  }

  if (this->media_pipeline_ != nullptr) {
    while (this->media_pipeline_->read_event(&event)) {
      // if (this->media_pipeline_->read_event(&event)) {
      switch (event.type) {
        case EventType::STARTING:
          ESP_LOGD(TAG, "Starting Media Playback");
          this->media_pipeline_state_ = PipelineState::STARTING;
          break;
        case EventType::STARTED:
          ESP_LOGD(TAG, "Started Media Playback");
          this->media_pipeline_state_ = PipelineState::STARTED;
          break;
        case EventType::IDLE:
          this->media_pipeline_state_ = PipelineState::PLAYING;
          break;
        case EventType::RUNNING:
          this->media_pipeline_state_ = PipelineState::PLAYING;
          this->status_clear_warning();
          break;
        case EventType::STOPPING:
          this->media_pipeline_state_ = PipelineState::STOPPING;
          ESP_LOGD(TAG, "Stopping Media Playback");
          break;
        case EventType::STOPPED:
          this->media_pipeline_state_ = PipelineState::STOPPED;
          this->media_pipeline_->stop();

          ESP_LOGD(TAG, "Stopped Media Playback");
          break;
        case EventType::WARNING:
          ESP_LOGW(TAG, "Error reading media: %s", esp_err_to_name(event.err));
          this->status_set_warning(esp_err_to_name(event.err));
          break;
      }
    }
  }
  if (this->combine_streamer_ != nullptr) {
    while (this->combine_streamer_->read_event(&event))
      ;
  }
}

// size_t transfer_data_(OutputStreamer *source_streamer, size_t free, uint8_t *buffer) {
//   if ((source_streamer == nullptr) || (source_streamer->available() == 0)) {
//     return 0;
//   }
//   return source_streamer->read(buffer, free);
// }

void NabuMediaPlayer::loop() {
  size_t bytes_read = 0;

  this->watch_media_commands_();
  this->watch_();

  // Determine state of the media player
  media_player::MediaPlayerState old_state = this->state;

  if ((this->announcement_pipeline_state_ != PipelineState::STOPPING) &&
      (this->announcement_pipeline_state_ != PipelineState::STOPPED)) {
    this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
  } else {
    if (this->is_paused_) {
      this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
    } else if ((this->media_pipeline_state_ == PipelineState::STOPPING) ||
               (this->media_pipeline_state_ == PipelineState::STOPPED)) {
      this->state = media_player::MEDIA_PLAYER_STATE_IDLE;
    } else {
      this->state = media_player::MEDIA_PLAYER_STATE_PLAYING;
    }
  }

  if (this->state != old_state) {
    this->publish_state();
  }
}

void NabuMediaPlayer::set_ducking_ratio(float ducking_ratio) {
  if (this->combine_streamer_ != nullptr) {
    CommandEvent command_event;
    command_event.command = CommandEventType::DUCK;
    command_event.ducking_ratio = ducking_ratio;
    this->combine_streamer_->send_command(&command_event);
  }
}

void NabuMediaPlayer::control(const media_player::MediaPlayerCall &call) {
  MediaCallCommand media_command;

  if (call.get_media_url().has_value()) {
    std::string new_uri = call.get_media_url().value();

    if (call.get_announcement().has_value() && call.get_announcement().value()) {
      this->announcement_url_ = new_uri;
      media_command.new_url = true;
      media_command.announce = true;
    } else {
      this->media_url_ = new_uri;
      media_command.new_url = true;
      media_command.announce = false;
    }
    xQueueSend(this->media_control_command_queue_, &media_command, portMAX_DELAY);
    return;
  }

  if (call.get_volume().has_value()) {
    media_command.volume = call.get_volume().value();
    xQueueSend(this->media_control_command_queue_, &media_command, portMAX_DELAY);
    return;
  }

  if (call.get_command().has_value()) {
    media_command.command = call.get_command().value();
    xQueueSend(this->media_control_command_queue_, &media_command, portMAX_DELAY);
    return;
  }
}

// pausing is only supported if destroy_pipeline_on_stop is disabled
media_player::MediaPlayerTraits NabuMediaPlayer::get_traits() {
  auto traits = media_player::MediaPlayerTraits();
  traits.set_supports_pause(true);
  return traits;
};

}  // namespace nabu
}  // namespace esphome
#endif

#include "nabu_media_player.h"

#ifdef USE_ESP_IDF

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace nabu {

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
static const size_t TRANSFER_BUFFER_SIZE = 8192;

void NabuMediaPlayer::setup() {
  state = media_player::MEDIA_PLAYER_STATE_IDLE;

  ExternalRAMAllocator<uint8_t> allocator(ExternalRAMAllocator<uint8_t>::ALLOW_FAILURE);
  this->transfer_buffer_ = allocator.allocate(TRANSFER_BUFFER_SIZE * sizeof(int16_t));
  if (this->transfer_buffer_ == nullptr) {
    ESP_LOGW(TAG, "Could not allocate transfer buffer");
    this->mark_failed();
    return;
  }

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
        }
      }
      this->combine_streamer_->start();

      if (media_command.announce.has_value() && media_command.announce.value()) {
        if (this->announcement_pipeline_ == nullptr) {
          this->announcement_pipeline_ =
              make_unique<Pipeline>(this->combine_streamer_.get(), PipelineType::ANNOUNCEMENT);
        }
        if (this->announcement_pipeline_state_ != PipelineState::STOPPED) {
          command_event.command = CommandEventType::STOP;
          this->announcement_pipeline_->send_command(&command_event);
          // WON"T PROPERLY RESTART

        } else {
          this->announcement_pipeline_->start(1, this->announcement_url_.value());
        }
      } else {
        if (this->media_pipeline_ == nullptr) {
          this->media_pipeline_ = make_unique<Pipeline>(this->combine_streamer_.get(), PipelineType::MEDIA);
        }
        if (this->media_pipeline_state_ != PipelineState::STOPPED) {
          command_event.command = CommandEventType::STOP;
          this->media_pipeline_->send_command(&command_event);
          // WON"T PROPERLY RESTART
        } else {
          this->media_pipeline_->start(1, this->media_url_.value());
        }
      }
      this->is_paused_ = false;
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
            this->is_paused_= false;
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
    if (this->announcement_pipeline_->read_event(&event)) {
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
          this->status_set_warning();
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
          this->status_set_warning();
          break;
      }
    }
  }
  if (this->combine_streamer_ != nullptr) {
    while (this->combine_streamer_->read_event(&event))
      ;
  }
}

size_t transfer_data_(OutputStreamer *source_streamer, size_t free, uint8_t *buffer) {
  if ((source_streamer == nullptr) || (source_streamer->available() == 0)) {
    return 0;
  }
  return source_streamer->read(buffer, free);
}

void NabuMediaPlayer::loop() {
  size_t bytes_read = 0;

  bytes_read = transfer_data_(this->combine_streamer_.get(), this->speaker_->free_bytes(), this->transfer_buffer_);
  if (bytes_read > 0) {
    size_t bytes_written = this->speaker_->write(this->transfer_buffer_, bytes_read);
  }

  this->watch_media_commands_();
  this->watch_();

  // Determine state of the media player
  media_player::MediaPlayerState old_state = this->state;

  if ((this->announcement_pipeline_state_ != PipelineState::STOPPING) && (this->announcement_pipeline_state_ != PipelineState::STOPPED)) {
    this->state = media_player::MEDIA_PLAYER_STATE_ANNOUNCING;
  }
  else {
    if (this->is_paused_) {
      this->state = media_player::MEDIA_PLAYER_STATE_PAUSED;
    } else if ((this->media_pipeline_state_ == PipelineState::STOPPING) || (this->media_pipeline_state_ == PipelineState::STOPPED)) {
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

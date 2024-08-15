#pragma once

#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace media_player {

struct StreamInfo {
  bool operator==(const StreamInfo &rhs) const {
    return (channels == rhs.channels) && (bits_per_sample == rhs.bits_per_sample) && (sample_rate == rhs.sample_rate);
  }
  bool operator!=(const StreamInfo &rhs) const { return !operator==(rhs); }
  uint8_t channels = 1;
  uint8_t bits_per_sample = 16;
  uint32_t sample_rate = 16000;
};

enum MediaPlayerState : uint8_t {
  MEDIA_PLAYER_STATE_NONE = 0,
  MEDIA_PLAYER_STATE_IDLE = 1,
  MEDIA_PLAYER_STATE_PLAYING = 2,
  MEDIA_PLAYER_STATE_PAUSED = 3,
  MEDIA_PLAYER_STATE_ANNOUNCING = 4
};
const char *media_player_state_to_string(MediaPlayerState state);

enum MediaPlayerCommand : uint8_t {
  MEDIA_PLAYER_COMMAND_PLAY = 0,
  MEDIA_PLAYER_COMMAND_PAUSE = 1,
  MEDIA_PLAYER_COMMAND_STOP = 2,
  MEDIA_PLAYER_COMMAND_MUTE = 3,
  MEDIA_PLAYER_COMMAND_UNMUTE = 4,
  MEDIA_PLAYER_COMMAND_TOGGLE = 5,
  MEDIA_PLAYER_COMMAND_VOLUME_UP = 6,
  MEDIA_PLAYER_COMMAND_VOLUME_DOWN = 7,
};
const char *media_player_command_to_string(MediaPlayerCommand command);

enum class MediaFileType : uint8_t {
  NONE = 0,
  WAV,
  MP3,
  FLAC,
};
const char *media_player_file_type_to_string(MediaFileType file_type);

struct MediaFile {
  const uint8_t *data;
  size_t length;
  MediaFileType file_type;
};

<<<<<<< Updated upstream
=======
enum class MediaPlayerFormatPurpose : uint8_t {
  DEFAULT = 0,
  ANNOUNCEMENT,
};

struct MediaPlayerSupportedFormat {
  std::string format;
  uint32_t sample_rate;
  uint32_t num_channels;
  MediaPlayerFormatPurpose purpose;
};


>>>>>>> Stashed changes
class MediaPlayer;

class MediaPlayerTraits {
 public:
  MediaPlayerTraits() = default;

  void set_supports_pause(bool supports_pause) { this->supports_pause_ = supports_pause; }

  bool get_supports_pause() const { return this->supports_pause_; }

  std::vector<MediaPlayerSupportedFormat>& get_supported_formats() {
    return this->supported_formats_;
  }

 protected:
  bool supports_pause_{false};
  std::vector<MediaPlayerSupportedFormat> supported_formats_{};
};

class MediaPlayerCall {
 public:
  MediaPlayerCall(MediaPlayer *parent) : parent_(parent) {}

  MediaPlayerCall &set_command(MediaPlayerCommand command);
  MediaPlayerCall &set_command(optional<MediaPlayerCommand> command);
  MediaPlayerCall &set_command(const std::string &command);

  MediaPlayerCall &set_media_url(const std::string &url);
  MediaPlayerCall &set_local_media_file(MediaFile *media_file);

  MediaPlayerCall &set_volume(float volume);
  MediaPlayerCall &set_announcement(bool announce);

  void perform();

  const optional<MediaPlayerCommand> &get_command() const { return command_; }
  const optional<std::string> &get_media_url() const { return media_url_; }
  const optional<float> &get_volume() const { return volume_; }
  const optional<bool> &get_announcement() const { return announcement_; }
  const optional<MediaFile *> &get_local_media_file() const { return media_file_; }

 protected:
  void validate_();
  MediaPlayer *const parent_;
  optional<MediaPlayerCommand> command_;
  optional<std::string> media_url_;
  optional<float> volume_;
  optional<bool> announcement_;
  optional<MediaFile *> media_file_;
};

class MediaPlayer : public EntityBase {
 public:
  MediaPlayerState state{MEDIA_PLAYER_STATE_NONE};
  float volume{1.0f};

  MediaPlayerCall make_call() { return MediaPlayerCall(this); }

  void publish_state();

  void add_on_state_callback(std::function<void()> &&callback);

  virtual bool is_muted() const { return false; }

  virtual MediaPlayerTraits get_traits() = 0;

 protected:
  friend MediaPlayerCall;

  virtual void control(const MediaPlayerCall &call) = 0;

  CallbackManager<void()> state_callback_{};
};

}  // namespace media_player
}  // namespace esphome

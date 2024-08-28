#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace audio_dac {

class AudioDac : public Component {
#ifdef USE_AUDIO_DAC
 public:
  virtual bool set_mute_off() = 0;
  virtual bool set_mute_on() = 0;
  virtual bool set_volume(optional<float> volume) = 0;

  virtual bool is_muted() = 0;
  virtual float volume() = 0;

 protected:
  bool is_muted_{false};
#endif
};

}  // namespace aic3204
}  // namespace esphome

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace audio_dac {

class AudioDac : public Component {
 public:
  virtual void set_mute_off(){ this->is_muted = false; }
  virtual void set_mute_on(){ this->is_muted = true; }
  virtual void set_volume(optional<float> volume){}

 protected:
  bool is_muted{false};
};

}  // namespace aic3204
}  // namespace esphome

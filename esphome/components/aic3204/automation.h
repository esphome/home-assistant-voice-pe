#pragma once

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "aic3204.h"

namespace esphome {
namespace aic3204 {

#ifdef USE_AUDIO_PROCESSOR
template<typename... Ts> class MuteOffAction : public Action<Ts...> {
 public:
  explicit MuteOffAction(AIC3204 *aic3204) : aic3204_(aic3204) {}

  void play(Ts... x) override { this->aic3204_->set_mute_off(); }

 protected:
  AIC3204 *aic3204_;
};

template<typename... Ts> class MuteOnAction : public Action<Ts...> {
 public:
  explicit MuteOnAction(AIC3204 *aic3204) : aic3204_(aic3204) {}

  void play(Ts... x) override { this->aic3204_->set_mute_on(); }

 protected:
  AIC3204 *aic3204_;
};

template<typename... Ts> class SetAutoMuteAction : public Action<Ts...> {
 public:
  explicit SetAutoMuteAction(AIC3204 *aic3204) : aic3204_(aic3204) {}

  TEMPLATABLE_VALUE(uint8_t, auto_mute_mode)

  void play(Ts... x) override { this->aic3204_->set_auto_mute_mode(this->auto_mute_mode_.optional_value(x...)); }

 protected:
  AIC3204 *aic3204_;
};

template<typename... Ts> class SetVolumeAction : public Action<Ts...> {
 public:
  explicit SetVolumeAction(AIC3204 *aic3204) : aic3204_(aic3204) {}

  TEMPLATABLE_VALUE(float, volume)

  void play(Ts... x) override { this->aic3204_->set_volume(this->volume_.optional_value(x...)); }

 protected:
  AIC3204 *aic3204_;
};
#endif

}  // namespace aic3204
}  // namespace esphome

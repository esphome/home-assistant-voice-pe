#pragma once
#include "voice_kit.h"

#include "esphome/core/automation.h"

namespace esphome {
namespace voice_kit {

template<typename... Ts> class VoiceKitFlashAction : public Action<Ts...> {
 public:
  VoiceKitFlashAction(VoiceKit *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->flash(); }

 protected:
  VoiceKit *parent_;
};

}  // namespace voice_kit
}  // namespace esphome

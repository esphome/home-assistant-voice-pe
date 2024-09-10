#pragma once
#include "voice_kit.h"

#include "esphome/core/automation.h"

namespace esphome {
namespace voice_kit {

template<typename... Ts> class VoiceKitFlashAction : public Action<Ts...> {
 public:
  VoiceKitFlashAction(VoiceKit *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->start_dfu_update(); }

 protected:
  VoiceKit *parent_;
};
#ifdef USE_VOICE_KIT_STATE_CALLBACK
class DFUStartTrigger : public Trigger<> {
 public:
  explicit DFUStartTrigger(VoiceKit *parent) {
    parent->add_on_state_callback(
        [this, parent](DFUAutomationState state, float progress, VoiceKitUpdaterStatus error) {
          if (state == DFU_START && !parent->is_failed()) {
            trigger();
          }
        });
  }
};

class DFUProgressTrigger : public Trigger<float> {
 public:
  explicit DFUProgressTrigger(VoiceKit *parent) {
    parent->add_on_state_callback(
        [this, parent](DFUAutomationState state, float progress, VoiceKitUpdaterStatus error) {
          if (state == DFU_IN_PROGRESS && !parent->is_failed()) {
            trigger(progress);
          }
        });
  }
};

class DFUEndTrigger : public Trigger<> {
 public:
  explicit DFUEndTrigger(VoiceKit *parent) {
    parent->add_on_state_callback(
        [this, parent](DFUAutomationState state, float progress, VoiceKitUpdaterStatus error) {
          if (state == DFU_COMPLETE && !parent->is_failed()) {
            trigger();
          }
        });
  }
};

class DFUErrorTrigger : public Trigger<uint8_t> {
 public:
  explicit DFUErrorTrigger(VoiceKit *parent) {
    parent->add_on_state_callback(
        [this, parent](DFUAutomationState state, float progress, VoiceKitUpdaterStatus error) {
          if (state == DFU_ERROR && !parent->is_failed()) {
            trigger(error);
          }
        });
  }
};
#endif
}  // namespace voice_kit
}  // namespace esphome

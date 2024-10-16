#pragma once

#ifdef USE_ESP_IDF

#include "preprocessor_settings.h"
#include "streaming_model.h"

#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#include "esphome/components/microphone/microphone.h"

#include <frontend_util.h>

#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

#include <freertos/event_groups.h>
namespace esphome {
namespace micro_wake_word {

enum State {
  IDLE,
  DETECTING_WAKE_WORD,
};

class MicroWakeWord : public Component {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override;
  void dump_config() override;

  void start();
  void stop();

  bool is_running() const { return this->state_ != State::IDLE; }

  void set_features_step_size(uint8_t step_size) { this->features_step_size_ = step_size; }

  void set_microphone(microphone::Microphone *microphone) { this->microphone_ = microphone; }

  Trigger<std::string> *get_wake_word_detected_trigger() const { return this->wake_word_detected_trigger_; }

  void add_wake_word_model(WakeWordModel *model);

#ifdef USE_MICRO_WAKE_WORD_VAD
  void add_vad_model(const uint8_t *model_start, uint8_t probability_cutoff, size_t sliding_window_size,
                     size_t tensor_arena_size);

  // Intended for the voice assistant component to fetch VAD status
  bool get_vad_state() { return this->vad_state_; }
#endif

  // Intended for the voice assistant component to know which wake words are available
  // Since these are pointers to the WakeWordModel objects, the voice assistant component can enable or disable them
  std::vector<WakeWordModel *> get_wake_words();

 protected:
  microphone::Microphone *microphone_{nullptr};
  Trigger<std::string> *wake_word_detected_trigger_ = new Trigger<std::string>();
  State state_{State::IDLE};

  std::vector<WakeWordModel *> wake_word_models_;

#ifdef USE_MICRO_WAKE_WORD_VAD
  std::unique_ptr<VADModel> vad_model_;
  bool vad_state_{false};
#endif

  // Audio frontend handles generating spectrogram features
  struct FrontendConfig frontend_config_;
  struct FrontendState frontend_state_;

  uint8_t features_step_size_;

  /// @brief Suspends the preprocessor and inference tasks
  void suspend_tasks_();
  /// @brief Resumes the preprocessor and inference tasks
  void resume_tasks_();

  void set_state_(State state);

  /// @brief Deletes each model's TFLite interpreters and frees tensor arena memory. Frees memory used by the feature
  /// generation frontend.
  void unload_models_();

  /** Performs inference with each configured model
   *
   * If enough audio samples are available, it will generate one slice of new features.
   * It then loops through and performs inference with each of the loaded models.
   */
  bool update_model_probabilities_();

  inline uint16_t new_samples_to_get_() { return (this->features_step_size_ * (AUDIO_SAMPLE_FREQUENCY / 1000)); }

  // Handles managing the start/stop/state of the preprocessor and inference tasks
  EventGroupHandle_t event_group_;

  // Used to send messages about the model's states to the main loop
  QueueHandle_t detection_queue_;

  // Stores spectrogram features for inference
  QueueHandle_t features_queue_;

  static void preprocessor_task_(void *params);
  TaskHandle_t preprocessor_task_handle_{nullptr};
  StaticTask_t preprocessor_task_stack_;
  StackType_t *preprocessor_task_stack_buffer_{nullptr};

  static void inference_task_(void *params);
  TaskHandle_t inference_task_handle_{nullptr};
  StaticTask_t inference_task_stack_;
  StackType_t *inference_task_stack_buffer_{nullptr};
};

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<MicroWakeWord> {
 public:
  void play(Ts... x) override { this->parent_->start(); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<MicroWakeWord> {
 public:
  void play(Ts... x) override { this->parent_->stop(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<MicroWakeWord> {
 public:
  bool check(Ts... x) override { return this->parent_->is_running(); }
};

}  // namespace micro_wake_word
}  // namespace esphome

#endif  // USE_ESP_IDF

#include "micro_wake_word.h"
#include "streaming_model.h"

#ifdef USE_ESP_IDF

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_OTA
#include "esphome/components/ota/ota_backend.h"
#endif

#include <frontend.h>
#include <frontend_util.h>

#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

#include <cmath>

namespace esphome {
namespace micro_wake_word {

static const char *const TAG = "micro_wake_word";

static const ssize_t DETECTION_QUEUE_COUNT = 5;

static const ssize_t FEATURES_QUEUE_LENGTH = 10;

// How long to block tasks while waiting for audio or spectrogram features data
static const size_t DATA_TIMEOUT_MS = 50;
static const size_t STOPPING_TIMEOUT_MS = 200;

static const uint32_t PREPROCESSOR_TASK_STACK_SIZE = 3072;
static const uint32_t INFERENCE_TASK_STACK_SIZE = 3072;

static const UBaseType_t PREPROCESSOR_TASK_PRIORITY = 3;
static const UBaseType_t INFERENCE_TASK_PRIORITY = 3;

enum EventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),  // Stops all activity in the mWW tasks

  PREPROCESSOR_COMMAND_START = (1 << 4),
  PREPROCESSOR_MESSAGE_STARTED = (1 << 5),
  PREPROCESSOR_MESSAGE_IDLE = (1 << 6),
  PREPROCESSOR_MESSAGE_ERROR = (1 << 7),
  PREPROCESSOR_MESSAGE_WARNING_FEATURES_FULL = (1 << 8),

  INFERENCE_MESSAGE_STARTED = (1 << 12),
  INFERENCE_MESSAGE_IDLE = (1 << 13),
  INFERENCE_MESSAGE_ERROR = (1 << 14),

  ALL_BITS = 0xfffff,  // 24 total bits available in an event group
};

float MicroWakeWord::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

static const LogString *micro_wake_word_state_to_string(State state) {
  switch (state) {
    case State::IDLE:
      return LOG_STR("IDLE");
    case State::DETECTING_WAKE_WORD:
      return LOG_STR("DETECTING_WAKE_WORD");
    default:
      return LOG_STR("UNKNOWN");
  }
}

void MicroWakeWord::dump_config() {
  ESP_LOGCONFIG(TAG, "microWakeWord:");
  ESP_LOGCONFIG(TAG, "  models:");
  for (auto &model : this->wake_word_models_) {
    model->log_model_config();
  }
#ifdef USE_MICRO_WAKE_WORD_VAD
  this->vad_model_->log_model_config();
#endif
}

void MicroWakeWord::setup() {
  ESP_LOGCONFIG(TAG, "Setting up microWakeWord...");

  this->frontend_config_.window.size_ms = FEATURE_DURATION_MS;
  this->frontend_config_.window.step_size_ms = this->features_step_size_;
  this->frontend_config_.filterbank.num_channels = PREPROCESSOR_FEATURE_SIZE;
  this->frontend_config_.filterbank.lower_band_limit = FILTERBANK_LOWER_BAND_LIMIT;
  this->frontend_config_.filterbank.upper_band_limit = FILTERBANK_UPPER_BAND_LIMIT;
  this->frontend_config_.noise_reduction.smoothing_bits = NOISE_REDUCTION_SMOOTHING_BITS;
  this->frontend_config_.noise_reduction.even_smoothing = NOISE_REDUCTION_EVEN_SMOOTHING;
  this->frontend_config_.noise_reduction.odd_smoothing = NOISE_REDUCTION_ODD_SMOOTHING;
  this->frontend_config_.noise_reduction.min_signal_remaining = NOISE_REDUCTION_MIN_SIGNAL_REMAINING;
  this->frontend_config_.pcan_gain_control.enable_pcan = PCAN_GAIN_CONTROL_ENABLE_PCAN;
  this->frontend_config_.pcan_gain_control.strength = PCAN_GAIN_CONTROL_STRENGTH;
  this->frontend_config_.pcan_gain_control.offset = PCAN_GAIN_CONTROL_OFFSET;
  this->frontend_config_.pcan_gain_control.gain_bits = PCAN_GAIN_CONTROL_GAIN_BITS;
  this->frontend_config_.log_scale.enable_log = LOG_SCALE_ENABLE_LOG;
  this->frontend_config_.log_scale.scale_shift = LOG_SCALE_SCALE_SHIFT;

  this->event_group_ = xEventGroupCreate();
  this->detection_queue_ = xQueueCreate(DETECTION_QUEUE_COUNT, sizeof(DetectionEvent));

  this->features_queue_ = xQueueCreate(FEATURES_QUEUE_LENGTH, PREPROCESSOR_FEATURE_SIZE * sizeof(int8_t));

  this->preprocessor_task_stack_buffer_ = (StackType_t *) malloc(PREPROCESSOR_TASK_STACK_SIZE);
  this->inference_task_stack_buffer_ = (StackType_t *) malloc(INFERENCE_TASK_STACK_SIZE);

  ESP_LOGCONFIG(TAG, "Micro Wake Word initialized");

#ifdef USE_OTA
  ota::get_global_ota_callback()->add_on_state_callback(
      [this](ota::OTAState state, float progress, uint8_t error, ota::OTAComponent *comp) {
        if (state == ota::OTA_STARTED) {
          this->suspend_tasks_();
        } else if (state == ota::OTA_ERROR) {
          this->resume_tasks_();
        }
      });
#endif
}

void MicroWakeWord::preprocessor_task_(void *params) {
  MicroWakeWord *this_mww = (MicroWakeWord *) params;

  while (true) {
    xEventGroupSetBits(this_mww->event_group_, EventGroupBits::PREPROCESSOR_MESSAGE_IDLE);

    EventBits_t event_bits = xEventGroupWaitBits(this_mww->event_group_,
                                                 PREPROCESSOR_COMMAND_START,  // Bit message to read
                                                 pdTRUE,                      // Clear the bit on exit
                                                 pdFALSE,                     // Wait for all the bits
                                                 portMAX_DELAY);              // Block indefinitely until bit is set

    xEventGroupClearBits(this_mww->event_group_, EventGroupBits::PREPROCESSOR_MESSAGE_IDLE);
    {
      // Setup preprocesor feature generator
      if (!FrontendPopulateState(&this_mww->frontend_config_, &this_mww->frontend_state_, AUDIO_SAMPLE_FREQUENCY)) {
        FrontendFreeStateContents(&this_mww->frontend_state_);
        xEventGroupSetBits(this_mww->event_group_,
                           EventGroupBits::PREPROCESSOR_MESSAGE_ERROR | EventGroupBits::COMMAND_STOP);
      }

      const size_t new_samples_to_read = this_mww->features_step_size_ * (AUDIO_SAMPLE_FREQUENCY / 1000);

      ExternalRAMAllocator<int16_t> int16_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);

      int8_t features_buffer[PREPROCESSOR_FEATURE_SIZE];
      int16_t *audio_buffer = int16_allocator.allocate(new_samples_to_read);

      if (audio_buffer == nullptr) {
        xEventGroupSetBits(this_mww->event_group_,
                           EventGroupBits::PREPROCESSOR_MESSAGE_ERROR | EventGroupBits::COMMAND_STOP);
      }

      if (this_mww->microphone_->is_stopped()) {
        this_mww->microphone_->start();
      }

      if (!(xEventGroupGetBits(this_mww->event_group_) & EventGroupBits::PREPROCESSOR_MESSAGE_ERROR)) {
        xEventGroupSetBits(this_mww->event_group_, EventGroupBits::PREPROCESSOR_MESSAGE_STARTED);
      }

      while (!(xEventGroupGetBits(this_mww->event_group_) & COMMAND_STOP)) {
        size_t bytes_read = this_mww->microphone_->read(audio_buffer, new_samples_to_read * sizeof(int16_t),
                                                        pdMS_TO_TICKS(DATA_TIMEOUT_MS));
        if (bytes_read < new_samples_to_read * sizeof(int16_t)) {
          // This shouldn't ever happen, but if we somehow don't have enough samples, just drop this frame
          continue;
        }

        size_t num_samples_processed;
        struct FrontendOutput frontend_output = FrontendProcessSamples(&this_mww->frontend_state_, audio_buffer,
                                                                       new_samples_to_read, &num_samples_processed);

        for (size_t i = 0; i < frontend_output.size; ++i) {
          // These scaling values are set to match the TFLite audio frontend int8 output.
          // The feature pipeline outputs 16-bit signed integers in roughly a 0 to 670
          // range. In training, these are then arbitrarily divided by 25.6 to get
          // float values in the rough range of 0.0 to 26.0. This scaling is performed
          // for historical reasons, to match up with the output of other feature
          // generators.
          // The process is then further complicated when we quantize the model. This
          // means we have to scale the 0.0 to 26.0 real values to the -128 (INT8_MIN)
          // to 127 (INT8_MAX) signed integer numbers.
          // All this means that to get matching values from our integer feature
          // output into the tensor input, we have to perform:
          // input = (((feature / 25.6) / 26.0) * 256) - 128
          // To simplify this and perform it in 32-bit integer math, we rearrange to:
          // input = (feature * 256) / (25.6 * 26.0) - 128
          constexpr int32_t value_scale = 256;
          constexpr int32_t value_div = 666;  // 666 = 25.6 * 26.0 after rounding
          int32_t value = ((frontend_output.values[i] * value_scale) + (value_div / 2)) / value_div;

          value -= INT8_MIN;
          features_buffer[i] = clamp<int8_t>(value, INT8_MIN, INT8_MAX);
        }

        if (!xQueueSendToBack(this_mww->features_queue_, features_buffer, 0)) {
          // Features queue is too full, so we fell behind on inferring!

          xEventGroupSetBits(this_mww->event_group_, EventGroupBits::PREPROCESSOR_MESSAGE_WARNING_FEATURES_FULL);
        }
      }

      this_mww->microphone_->stop();

      FrontendFreeStateContents(&this_mww->frontend_state_);

      if (audio_buffer != nullptr) {
        int16_allocator.deallocate(audio_buffer, new_samples_to_read);
      }
    }
  }
}

std::vector<WakeWordModel *> MicroWakeWord::get_wake_words() {
  std::vector<WakeWordModel *> external_wake_word_models;
  for (auto model : this->wake_word_models_) {
    if (!model->get_internal_only()) {
      external_wake_word_models.push_back(model);
    }
  }
  return external_wake_word_models;
}

void MicroWakeWord::inference_task_(void *params) {
  MicroWakeWord *this_mww = (MicroWakeWord *) params;

  while (true) {
    xEventGroupSetBits(this_mww->event_group_, EventGroupBits::INFERENCE_MESSAGE_IDLE);

    EventBits_t event_bits = xEventGroupWaitBits(this_mww->event_group_,
                                                 PREPROCESSOR_MESSAGE_STARTED,  // Bit message to read
                                                 pdTRUE,                        // Clear the bit on exit
                                                 pdFALSE,                       // Wait for all the bits,
                                                 portMAX_DELAY);                // Block indefinitely until bit is set

    xEventGroupClearBits(this_mww->event_group_, EventGroupBits::INFERENCE_MESSAGE_IDLE);

    {
      xEventGroupSetBits(this_mww->event_group_, EventGroupBits::INFERENCE_MESSAGE_STARTED);

      while (!(xEventGroupGetBits(this_mww->event_group_) & COMMAND_STOP)) {
        if (!this_mww->update_model_probabilities_()) {
          // Ran into an issue with inference
          xEventGroupSetBits(this_mww->event_group_,
                             EventGroupBits::INFERENCE_MESSAGE_ERROR | EventGroupBits::COMMAND_STOP);
        }

#ifdef USE_MICRO_WAKE_WORD_VAD
        DetectionEvent vad_state = this_mww->vad_model_->determine_detected();

        this_mww->vad_state_ = vad_state.detected;  // atomic write, so thread safe
#endif

        for (auto &model : this_mww->wake_word_models_) {
          if (model->get_unprocessed_probability_status()) {
            // Only detect wake words if there is a new probability since the last check
            DetectionEvent wake_word_state = model->determine_detected();
            if (wake_word_state.detected) {
#ifdef USE_MICRO_WAKE_WORD_VAD
              if (vad_state.detected) {
#endif
                xQueueSend(this_mww->detection_queue_, &wake_word_state, portMAX_DELAY);
                model->reset_probabilities();
#ifdef USE_MICRO_WAKE_WORD_VAD
              } else {
                wake_word_state.blocked_by_vad = true;
                xQueueSend(this_mww->detection_queue_, &wake_word_state, portMAX_DELAY);
              }
#endif
            }
          }
        }
      }

      this_mww->unload_models_();
    }
  }
}

void MicroWakeWord::add_wake_word_model(WakeWordModel *model) { this->wake_word_models_.push_back(model); }

#ifdef USE_MICRO_WAKE_WORD_VAD
void MicroWakeWord::add_vad_model(const uint8_t *model_start, uint8_t probability_cutoff, size_t sliding_window_size,
                                  size_t tensor_arena_size) {
  this->vad_model_ = make_unique<VADModel>(model_start, probability_cutoff, sliding_window_size, tensor_arena_size);
}
#endif

void MicroWakeWord::suspend_tasks_() {
  if (this->preprocessor_task_handle_ != nullptr) {
    vTaskSuspend(this->preprocessor_task_handle_);
  }
  if (this->inference_task_handle_ != nullptr) {
    vTaskSuspend(this->inference_task_handle_);
  }
}

void MicroWakeWord::resume_tasks_() {
  if (this->preprocessor_task_handle_ != nullptr) {
    vTaskResume(this->preprocessor_task_handle_);
  }
  if (this->inference_task_handle_ != nullptr) {
    vTaskResume(this->inference_task_handle_);
  }
}

void MicroWakeWord::loop() {
  // Determines the state of microWakeWord by monitoring the Event Group state.
  // This is the only place where the component's state is modified
  if ((this->preprocessor_task_handle_ == nullptr) || (this->inference_task_handle_ == nullptr)) {
    this->set_state_(State::IDLE);
    return;
  }

  uint32_t event_bits = xEventGroupGetBits(this->event_group_);
  if (event_bits & EventGroupBits::PREPROCESSOR_MESSAGE_ERROR) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::PREPROCESSOR_MESSAGE_ERROR);
    this->set_state_(State::IDLE);
    ESP_LOGE(TAG, "Preprocessor task encounted an error");
    return;
  }

  if (event_bits & EventGroupBits::PREPROCESSOR_MESSAGE_WARNING_FEATURES_FULL) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::PREPROCESSOR_MESSAGE_WARNING_FEATURES_FULL);
    ESP_LOGW(TAG, "Spectrogram features queue is full. Wake word detection accuracy will decrease temporarily.");
  }

  if (event_bits & EventGroupBits::INFERENCE_MESSAGE_ERROR) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::INFERENCE_MESSAGE_ERROR);
    this->set_state_(State::IDLE);
    ESP_LOGE(TAG, "Inference task encounted an error");
    return;
  }

  if ((event_bits & EventGroupBits::PREPROCESSOR_MESSAGE_IDLE) ||
      (event_bits & EventGroupBits::INFERENCE_MESSAGE_IDLE)) {
    this->set_state_(State::IDLE);
    return;
  }

  if (event_bits & EventGroupBits::INFERENCE_MESSAGE_STARTED) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::INFERENCE_MESSAGE_STARTED);
    this->set_state_(State::DETECTING_WAKE_WORD);
  }

  DetectionEvent detection_event;
  while (xQueueReceive(this->detection_queue_, &detection_event, 0)) {
    if (detection_event.blocked_by_vad) {
      ESP_LOGD(TAG, "Wake word model predicts '%s', but VAD model doesn't.", detection_event.wake_word->c_str());
    } else {
      constexpr float uint8_to_float_divisor = 255.0f;  // Converting a quantized uint8 probability to floating point
      ESP_LOGD(TAG, "Detected '%s' with sliding average probability is %.2f and max probability is %.2f",
               detection_event.wake_word->c_str(), (detection_event.average_probability / uint8_to_float_divisor),
               (detection_event.max_probability / uint8_to_float_divisor));
      this->wake_word_detected_trigger_->trigger(*detection_event.wake_word);
    }
  }
}

void MicroWakeWord::start() {
  if (!this->is_ready()) {
    ESP_LOGW(TAG, "Wake word detection can't start as the component hasn't been setup yet");
    return;
  }

  if (this->is_failed()) {
    ESP_LOGW(TAG, "Wake word component is marked as failed. Please check setup logs");
    return;
  }

  if (this->is_running()) {
    ESP_LOGW(TAG, "Wake word dection is already running");
    return;
  }

  ESP_LOGD(TAG, "Starting wake word detection");

  if (this->preprocessor_task_handle_ == nullptr) {
    this->preprocessor_task_handle_ = xTaskCreateStatic(
        MicroWakeWord::preprocessor_task_, "preprocessor", PREPROCESSOR_TASK_STACK_SIZE, (void *) this,
        PREPROCESSOR_TASK_PRIORITY, this->preprocessor_task_stack_buffer_, &this->preprocessor_task_stack_);
  }

  if (this->inference_task_handle_ == nullptr) {
    this->inference_task_handle_ =
        xTaskCreateStatic(MicroWakeWord::inference_task_, "inference", INFERENCE_TASK_STACK_SIZE, (void *) this,
                          INFERENCE_TASK_PRIORITY, this->inference_task_stack_buffer_, &this->inference_task_stack_);
  }

  xEventGroupSetBits(this->event_group_, PREPROCESSOR_COMMAND_START);
}

void MicroWakeWord::stop() {
  if (this->state_ == IDLE)
    return;

  ESP_LOGD(TAG, "Stopping wake word detection");

  xEventGroupSetBits(this->event_group_, COMMAND_STOP);

  xEventGroupWaitBits(this->event_group_,
                      (PREPROCESSOR_MESSAGE_IDLE | INFERENCE_MESSAGE_IDLE),  // Bit message to read
                      pdTRUE,                                                // Clear the bit on exit
                      pdTRUE,                                                // Wait for all the bits,
                      pdMS_TO_TICKS(STOPPING_TIMEOUT_MS));                   // Block to wait until the tasks stop

  xEventGroupClearBits(this->event_group_, ALL_BITS);
  xQueueReset(this->features_queue_);
  xQueueReset(this->detection_queue_);
}

void MicroWakeWord::set_state_(State state) {
  if (this->state_ != state) {
    ESP_LOGD(TAG, "State changed from %s to %s", LOG_STR_ARG(micro_wake_word_state_to_string(this->state_)),
             LOG_STR_ARG(micro_wake_word_state_to_string(state)));
    this->state_ = state;
  }
}

void MicroWakeWord::unload_models_() {
  for (auto &model : this->wake_word_models_) {
    model->unload_model();
  }
#ifdef USE_MICRO_WAKE_WORD_VAD
  this->vad_model_->unload_model();
#endif
}

bool MicroWakeWord::update_model_probabilities_() {
  int8_t audio_features[PREPROCESSOR_FEATURE_SIZE];

  bool success = true;
  if (xQueueReceive(this->features_queue_, &audio_features, pdMS_TO_TICKS(DATA_TIMEOUT_MS))) {
    for (auto &model : this->wake_word_models_) {
      // Perform inference
      success = success & model->perform_streaming_inference(audio_features);
    }
#ifdef USE_MICRO_WAKE_WORD_VAD
    success = success & this->vad_model_->perform_streaming_inference(audio_features);
#endif
  }

  return success;
}

}  // namespace micro_wake_word
}  // namespace esphome

#endif  // USE_ESP_IDF
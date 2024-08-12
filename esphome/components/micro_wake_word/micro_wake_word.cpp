#include "micro_wake_word.h"
#include "streaming_model.h"

#ifdef USE_ESP_IDF

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <frontend.h>
#include <frontend_util.h>

#include <tensorflow/lite/core/c/common.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>

#include <cmath>

// TODO:
//  - does VAD need to be a separate class?
//  - setup protocol between micro_wake_word and voice_assistant components
//    - voice assistant should handle all decisions, mWW should just send info
//    - Need to be able to send the details of each available wake word to voice assistants
//  - load trained languages from manifest to expose to voice assistant

namespace esphome {
namespace micro_wake_word {

static const char *const TAG = "micro_wake_word";

static const size_t BUFFER_LENGTH = 64;  // 0.064 seconds
static const size_t QUEUE_COUNT = 5;

enum EventGroupBits : uint32_t {
  COMMAND_STOP = (1 << 0),  // Stops all activity in the mWW tasks

  PREPROCESSOR_COMMAND_START = (1 << 4),
  PREPROCESSOR_MESSAGE_STARTED = (1 << 5),
  PREPROCESSOR_MESSAGE_IDLE = (1 << 6),
  PREPROCESSOR_MESSAGE_ERROR = (1 << 7),

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
  this->frontend_config_.filterbank.lower_band_limit = 125.0;
  this->frontend_config_.filterbank.upper_band_limit = 7500.0;
  this->frontend_config_.noise_reduction.smoothing_bits = 10;
  this->frontend_config_.noise_reduction.even_smoothing = 0.025;
  this->frontend_config_.noise_reduction.odd_smoothing = 0.06;
  this->frontend_config_.noise_reduction.min_signal_remaining = 0.05;
  this->frontend_config_.pcan_gain_control.enable_pcan = 1;
  this->frontend_config_.pcan_gain_control.strength = 0.95;
  this->frontend_config_.pcan_gain_control.offset = 80.0;
  this->frontend_config_.pcan_gain_control.gain_bits = 21;
  this->frontend_config_.log_scale.enable_log = 1;
  this->frontend_config_.log_scale.scale_shift = 6;

  this->event_group_ = xEventGroupCreate();
  this->detection_queue_ = xQueueCreate(QUEUE_COUNT, sizeof(DetectionEvent));

  // Only store most recent wake word detection
  this->wake_word_queue_ = xQueueCreate(1, sizeof(DetectionEvent));

  ExternalRAMAllocator<StackType_t> allocator(ExternalRAMAllocator<StackType_t>::ALLOW_FAILURE);
  this->preprocessor_task_stack_buffer_ = allocator.allocate(8192);
  this->inference_task_stack_buffer_ = allocator.allocate(8192);

  ESP_LOGCONFIG(TAG, "Micro Wake Word initialized");
}

void MicroWakeWord::preprocessor_task_(void *params) {
  MicroWakeWord *this_mww = (MicroWakeWord *) params;

  while (true) {
    xEventGroupSetBits(this_mww->event_group_, EventGroupBits::PREPROCESSOR_MESSAGE_IDLE);

    EventBits_t event_bits = xEventGroupWaitBits(this_mww->event_group_,
                                                 PREPROCESSOR_COMMAND_START,  // Bit message to read
                                                 pdTRUE,                      // Clear the bit on exit
                                                 pdFALSE,                     // Wait for all the bits,
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

      ExternalRAMAllocator<int8_t> int8_allocator(ExternalRAMAllocator<int8_t>::ALLOW_FAILURE);
      ExternalRAMAllocator<int16_t> int16_allocator(ExternalRAMAllocator<int16_t>::ALLOW_FAILURE);

      int8_t *features_buffer = int8_allocator.allocate(PREPROCESSOR_FEATURE_SIZE);
      int16_t *audio_buffer = int16_allocator.allocate(new_samples_to_read);

      if ((audio_buffer == nullptr) || (features_buffer == nullptr)) {
        xEventGroupSetBits(this_mww->event_group_,
                           EventGroupBits::PREPROCESSOR_MESSAGE_ERROR | EventGroupBits::COMMAND_STOP);
      }

      if (this_mww->features_ring_buffer_ == nullptr) {
        this_mww->features_ring_buffer_ = RingBuffer::create(PREPROCESSOR_FEATURE_SIZE * 10);  // TODO: Tweak this
        if (this_mww->features_ring_buffer_ == nullptr) {
          xEventGroupSetBits(this_mww->event_group_,
                             EventGroupBits::PREPROCESSOR_MESSAGE_ERROR | EventGroupBits::COMMAND_STOP);
        }
      }

      if (this_mww->microphone_->is_stopped()) {
        this_mww->microphone_->start();
      }

      if (!(xEventGroupGetBits(this_mww->event_group_) & EventGroupBits::PREPROCESSOR_MESSAGE_ERROR)) {
        xEventGroupSetBits(this_mww->event_group_, EventGroupBits::PREPROCESSOR_MESSAGE_STARTED);
      }

      while (!(xEventGroupGetBits(this_mww->event_group_) & COMMAND_STOP)) {
        while (this_mww->microphone_->available() / sizeof(int16_t) >= new_samples_to_read) {
          size_t bytes_read = this_mww->microphone_->read(audio_buffer, new_samples_to_read * sizeof(int16_t));
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
            // means we have to scale the 0.0 to 26.0 real values to the -128 to 127
            // signed integer numbers.
            // All this means that to get matching values from our integer feature
            // output into the tensor input, we have to perform:
            // input = (((feature / 25.6) / 26.0) * 256) - 128
            // To simplify this and perform it in 32-bit integer math, we rearrange to:
            // input = (feature * 256) / (25.6 * 26.0) - 128
            constexpr int32_t value_scale = 256;
            constexpr int32_t value_div = 666;  // 666 = 25.6 * 26.0 after rounding
            int32_t value = ((frontend_output.values[i] * value_scale) + (value_div / 2)) / value_div;
            value -= 128;
            if (value < -128) {
              value = -128;
            }
            if (value > 127) {
              value = 127;
            }
            features_buffer[i] = value;
          }

          this_mww->features_ring_buffer_->write((void *) features_buffer, PREPROCESSOR_FEATURE_SIZE);
        }

        // Block to give other tasks opportunity to run
        delay(10);
      }

      FrontendFreeStateContents(&this_mww->frontend_state_);

      if (features_buffer != nullptr) {
        int8_allocator.deallocate(features_buffer, PREPROCESSOR_FEATURE_SIZE);
      }

      if (audio_buffer != nullptr) {
        int16_allocator.deallocate(audio_buffer, new_samples_to_read);
      }
    }
  }
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
        while (this_mww->features_ring_buffer_->available() > PREPROCESSOR_FEATURE_SIZE) {
          if (!this_mww->update_model_probabilities_()) {
            // Ran into an issue with inference
            xEventGroupSetBits(this_mww->event_group_,
                               EventGroupBits::INFERENCE_MESSAGE_ERROR | EventGroupBits::COMMAND_STOP);
          }

#ifdef USE_MICRO_WAKE_WORD_VAD
          DetectionEvent vad_state = this_mww->vad_model_->determine_detected();

          // Atomic write, so thread safe
          this_mww->vad_status_ = vad_state.detected;
#endif

          for (auto &model : this_mww->wake_word_models_) {
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

        // Block to give other tasks opportunity to run
        delay(10);
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

optional<DetectionEvent> MicroWakeWord::get_wake_word_detection_event() {
  DetectionEvent detection_event;
  if (!xQueueReceive(this->wake_word_queue_, &detection_event, 0)) {
    // Nothing in queue, return nothing
    return {};
  }
  return detection_event;
}

void MicroWakeWord::loop() {
  // Determines the state of microWakeWord by monitoring the Event Group state.
  // This is the only place where the component's state is modified
  if ((this->preprocessor_task_handle_ == nullptr) || (this->inference_task_handle_ == nullptr)) {
    this->set_state_(State::IDLE);
    return;
  }

  uint32_t event_bits = xEventGroupGetBits(this->event_group_);
  if (event_bits & PREPROCESSOR_MESSAGE_ERROR) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::PREPROCESSOR_MESSAGE_ERROR);
    this->set_state_(State::IDLE);
    ESP_LOGE(TAG, "Preprocessor task encounted an error");
    return;
  }

  if (event_bits & INFERENCE_MESSAGE_ERROR) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::INFERENCE_MESSAGE_ERROR);
    this->set_state_(State::IDLE);
    ESP_LOGE(TAG, "Inference task encounted an error");
    return;
  }

  if ((event_bits & PREPROCESSOR_MESSAGE_IDLE) || (event_bits & INFERENCE_MESSAGE_IDLE)) {
    this->set_state_(State::IDLE);
    return;
  }

  if (event_bits & INFERENCE_MESSAGE_STARTED) {
    xEventGroupClearBits(this->event_group_, EventGroupBits::INFERENCE_MESSAGE_STARTED);
    this->set_state_(State::DETECTING_WAKE_WORD);
  }

  DetectionEvent detection_event;
  while (xQueueReceive(this->detection_queue_, &detection_event, 0)) {
    if (detection_event.blocked_by_vad) {
      ESP_LOGD(TAG, "Wake word model predicts '%s', but VAD model doesn't.", detection_event.wake_word->c_str());
    } else {
      ESP_LOGD(TAG, "Detected '%s' with sliding average probability is %.2f and max probability is %.2f",
               detection_event.wake_word->c_str(), (detection_event.average_probability / 255.0f),
               (detection_event.max_probability / 255.0f));
      this->wake_word_detected_trigger_->trigger(*detection_event.wake_word);
      xQueueOverwrite(this->wake_word_queue_, &detection_event);
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
    this->preprocessor_task_handle_ =
        xTaskCreateStatic(MicroWakeWord::preprocessor_task_, "preprocessor", 8192, (void *) this, 10,
                          this->preprocessor_task_stack_buffer_, &this->preprocessor_task_stack_);
  }

  if (this->inference_task_handle_ == nullptr) {
    this->inference_task_handle_ =
        xTaskCreateStatic(MicroWakeWord::inference_task_, "inference", 8192, (void *) this, 5,
                          this->inference_task_stack_buffer_, &this->inference_task_stack_);
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
                      true,                                                  // Wait for all the bits,
                      pdMS_TO_TICKS(200));  // Block temporarily before deleting each task

  xEventGroupClearBits(this->event_group_, ALL_BITS);
  this->features_ring_buffer_->reset();
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
  this->features_ring_buffer_->read((void *) audio_features, PREPROCESSOR_FEATURE_SIZE);

  for (auto &model : this->wake_word_models_) {
    // Perform inference
    success = success & model->perform_streaming_inference(audio_features);
  }
#ifdef USE_MICRO_WAKE_WORD_VAD
  success = success & this->vad_model_->perform_streaming_inference(audio_features);
#endif

  return success;
}

}  // namespace micro_wake_word
}  // namespace esphome

#endif  // USE_ESP_IDF

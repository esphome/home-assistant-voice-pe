#pragma once

#ifdef USE_ESP_IDF

#include "esphome/components/media_player/media_player.h"

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/ring_buffer.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace esphome {
namespace nabu {

// Mixes two incoming audio streams together
//  - The media stream intended for music playback
//    - Able to duck (made quieter)
//    - Able to pause
//  - The annoucnement stream is intended for TTS reponses or various beeps/sound effects
//    - Unable to duck
//    - Unable to pause
//  - Each stream has a corresponding input ring buffer. Retrieved via the `get_media_ring_buffer` and
//    `get_announcement_ring_buffer` functions
//  - The mixed audio is stored in the output ring buffer. Use the `available` and `read` functions to access
//  - The mixer runs as a FreeRTOS task
//    - The task reports its state using the TaskEvent queue. Regularly call the  `read_event` function to obtain the
//      state
//    - Commands are sent to the task using a the CommandEvent queue. Use the `send_command` function to do so.
//    - Use the `start` function to initiate. The `stop` function deletes the task, but be sure to send a STOP comman
//      first to avoid memory leaks.

enum class EventType : uint8_t {
  STARTING = 0,
  STARTED,
  RUNNING,
  IDLE,
  STOPPING,
  STOPPED,
  WARNING = 255,
};

// Used for reporting the state of the mixer task
struct TaskEvent {
  EventType type;
  esp_err_t err;
};

enum class CommandEventType : uint8_t {
  STOP,                // Stop mixing to prepare for stopping the mixing task
  DUCK,                // Duck the media audio
  PAUSE_MEDIA,         // Pauses the media stream
  RESUME_MEDIA,        // Resumes the media stream
  CLEAR_MEDIA,         // Resets the media ring buffer
  CLEAR_ANNOUNCEMENT,  // Resets the announcement ring buffer
};

// Used to send commands to the mixer task
struct CommandEvent {
  CommandEventType command;
  uint8_t decibel_reduction;
  size_t transition_samples = 0;
};

class AudioMixer {
 public:
  /// @brief Returns the number of bytes available to read from the ring buffer
  size_t available() { return this->output_ring_buffer_->available(); }

  /// @brief Reads from the output ring buffer
  /// @param buffer stores the read data
  /// @param length how many bytes requested to read from the ring buffer
  /// @return number of bytes actually read; will be less than length if not available in ring buffer
  size_t read(uint8_t *buffer, size_t length, TickType_t ticks_to_wait = 0);

  /// @brief Sends a CommandEvent to the command queue
  /// @param command Pointer to CommandEvent object to be sent
  /// @param ticks_to_wait The number of FreeRTOS ticks to wait for an event to appear on the queue. Defaults to 0.
  /// @return pdTRUE if successful, pdFALSE otherwises
  BaseType_t send_command(CommandEvent *command, TickType_t ticks_to_wait = portMAX_DELAY) {
    return xQueueSend(this->command_queue_, command, ticks_to_wait);
  }

  /// @brief Reads a TaskEvent from the event queue indicating its current status
  /// @param event Pointer to TaskEvent object to store the event in
  /// @param ticks_to_wait The number of FreeRTOS ticks to wait for an event to appear on the queue. Defaults to 0.
  /// @return pdTRUE if successful, pdFALSE otherwise
  BaseType_t read_event(TaskEvent *event, TickType_t ticks_to_wait = 0) {
    return xQueueReceive(this->event_queue_, event, ticks_to_wait);
  }

  /// @brief Starts the mixer task
  /// @param task_name FreeRTOS task name
  /// @param priority FreeRTOS task priority. Defaults to 1
  /// @return ESP_OK if successful, and error otherwise
  esp_err_t start(const std::string &task_name, UBaseType_t priority = 1);

  /// @brief Stops the mixer task and clears the queues
  void stop();

  /// @brief Retrieves the media stream's ring buffer pointer
  /// @return pointer to media ring buffer
  RingBuffer *get_media_ring_buffer() { return this->media_ring_buffer_.get(); }

  /// @brief Retrieves the announcement stream's ring buffer pointer
  /// @return pointer to announcement ring buffer
  RingBuffer *get_announcement_ring_buffer() { return this->announcement_ring_buffer_.get(); }

 protected:
  /// @brief Allocates the ring buffers, task stack, and queues
  /// @return ESP_OK if successful or an error otherwise
  esp_err_t allocate_buffers_();

  /// @brief Resets teh output, media, and anouncement ring buffers
  void reset_ring_buffers_();

  /// @brief Mixes the media and announcement samples. If the resulting audio clips, the media samples are first scaled.
  /// @param media_buffer buffer for media samples
  /// @param announcement_buffer buffer for announcement samples
  /// @param combination_buffer buffer for the mixed samples
  /// @param samples_to_mix number of samples in the media and annoucnement buffers to mix together
  void mix_audio_samples_without_clipping_(int16_t *media_buffer, int16_t *announcement_buffer,
                                           int16_t *combination_buffer, size_t samples_to_mix);

  /// @brief Scales audio samples
  /// @param audio_samples PCM int16 audio samples
  /// @param output_buffer Buffer to store the scaled samples
  /// @param scale_factor Q15 fixed point scaling factor
  /// @param samples_to_scale Number of samples to scale
  void scale_audio_samples_(int16_t *audio_samples, int16_t *output_buffer, int16_t scale_factor,
                            size_t samples_to_scale);

  static void audio_mixer_task_(void *params);
  TaskHandle_t task_handle_{nullptr};
  StaticTask_t task_stack_;
  StackType_t *stack_buffer_{nullptr};

  // Reports events from the mixer task
  QueueHandle_t event_queue_;

  // Stores commands to send the mixer task
  QueueHandle_t command_queue_;

  // Stores the mixed audio
  std::unique_ptr<RingBuffer> output_ring_buffer_;

  std::unique_ptr<RingBuffer> media_ring_buffer_;
  std::unique_ptr<RingBuffer> announcement_ring_buffer_;
};
}  // namespace nabu
}  // namespace esphome

#endif
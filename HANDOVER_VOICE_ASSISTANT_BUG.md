# Handover: Voice Assistant Microphone Restart Bug

## Summary

**Bug**: Voice PE starts listening before TTS audio finishes playing, causing the device to hear itself and re-trigger STT.

**Root Cause**: In `esphome/components/voice_assistant/voice_assistant.cpp`, the `RESPONSE_FINISHED` state transitions to `START_MICROPHONE` without waiting for the media player to finish playing.

**Fix Location**: `esphome/esphome` repository, file `esphome/components/voice_assistant/voice_assistant.cpp`

---

## Related Issues

- **Existing Issue**: https://github.com/esphome/home-assistant-voice-pe/issues/563
  - Title: "Microphone re-opens before announcement finishes playing in continuous conversation mode"
  - Status: Open
  - Created: 2026-03-20
  - Has 2 thumbs up reactions

- **Slack Thread**: https://openhomefoundation.slack.com/archives/C08MUM89GUW/p1779288998390289?thread_ts=1779287244.902389&cid=C08MUM89GUW

---

## Bug Details

### Symptoms

1. User configures Voice PE with:
   - OpenAI gpt-4o-transcribe for STT
   - Claude Sonnet for conversation engine
   - OpenAI TTS

2. When the assistant asks a follow-up question, the microphone starts listening **before** the TTS audio finishes playing

3. The device hears its own TTS output and re-triggers STT, causing conversation failures

### Log Evidence (from Slack)

**With OpenAI TTS (BROKEN)** - Only 2 seconds for 4+ second audio:
```
[16:32:34][D][voice_assistant:490]: Desired state set to STREAMING_RESPONSE
[16:32:34][D][voice_assistant:631]: Event Type: 2
[16:32:34][D][voice_assistant:773]: Assist Pipeline ended
[16:32:34][D][http_media_source:069]: Starting
[16:32:36][D][voice_assistant:483]: State changed from STREAMING_RESPONSE to RESPONSE_FINISHED
[16:32:36][D][voice_assistant:483]: State changed from RESPONSE_FINISHED to START_MICROPHONE
```

**With HA Cloud TTS (WORKING)** - 9 seconds matches audio length:
```
[16:44:04][D][http_media_source:069]: Starting
[16:44:13][D][voice_assistant:483]: State changed from STREAMING_RESPONSE to RESPONSE_FINISHED
```

### Why It Happens

- OpenAI TTS streams audio data faster than real-time playback
- The HTTP download completes in ~2 seconds
- But the audio file is 4+ seconds long
- The state machine sees "streaming complete" and transitions immediately
- No check exists for media player playback completion

---

## Technical Analysis

### State Machine Flow

```
STREAMING_RESPONSE 
    → (audio data streaming finishes)
    → RESPONSE_FINISHED 
    → (if continue_conversation_) 
    → START_MICROPHONE  ← BUG: No wait for media player!
```

### Current Code (voice_assistant.cpp)

```cpp
case State::RESPONSE_FINISHED: {
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    // Checks for speaker buffer - works for internal speaker
    if (this->speaker_buffer_size_ > 0) {
      this->write_speaker_();
      break;
    }
    if (this->speaker_->has_buffered_data() || this->speaker_->is_running()) {
      break;
    }
    // ... speaker cleanup
  }
#endif
  // BUG: When using media_player_ (not speaker_), no wait occurs!
  if (this->continue_conversation_) {
    this->set_state_(State::START_MICROPHONE, State::START_PIPELINE);
  } else {
    this->set_state_(State::IDLE, State::IDLE);
  }
  break;
}
```

### The Problem

When `speaker_` is `nullptr` (user configured `media_player:` instead of `speaker:`), the speaker buffer checks are skipped entirely. The code immediately checks `continue_conversation_` and transitions to `START_MICROPHONE`.

---

## Proposed Fix

Add a media player announcement check before transitioning to `START_MICROPHONE`:

```cpp
case State::RESPONSE_FINISHED: {
#ifdef USE_SPEAKER
  if (this->speaker_ != nullptr) {
    if (this->speaker_buffer_size_ > 0) {
      this->write_speaker_();
      break;
    }
    if (this->speaker_->has_buffered_data() || this->speaker_->is_running()) {
      break;
    }
    ESP_LOGD(TAG, "Speaker has finished outputting");
    this->speaker_->stop();
    this->cancel_timeout("speaker-timeout");
    this->cancel_timeout("playing");
    this->clear_buffers_();
    this->wait_for_stream_end_ = false;
    this->stream_ended_ = false;
    this->tts_stream_end_trigger_.trigger();
  }
#endif

  // === NEW CODE START ===
#ifdef USE_MEDIA_PLAYER
  // When using media_player (not internal speaker), wait for announcement to finish
  if (this->speaker_ == nullptr && this->media_player_ != nullptr) {
    if (this->media_player_->is_announcing()) {
      break;  // Keep waiting until media player finishes
    }
    ESP_LOGD(TAG, "Media player has finished announcing");
  }
#endif
  // === NEW CODE END ===

  if (this->continue_conversation_) {
    this->set_state_(State::START_MICROPHONE, State::START_PIPELINE);
  } else {
    this->set_state_(State::IDLE, State::IDLE);
  }
  break;
}
```

### Why This Fix Works

1. Only activates when `speaker_` is null AND `media_player_` is configured
2. Uses existing `is_announcing()` method from MediaPlayer component
3. Keeps looping in `RESPONSE_FINISHED` state until announcement completes
4. Minimal change, follows existing pattern for speaker checks

---

## Files to Modify

| File | Change |
|------|--------|
| `esphome/components/voice_assistant/voice_assistant.cpp` | Add media player check in `RESPONSE_FINISHED` case |

---

## Testing

1. Configure Voice PE with:
   ```yaml
   voice_assistant:
     media_player: external_media_player
     # ... other config
   ```

2. Use a TTS provider that streams faster than real-time (OpenAI TTS)

3. Trigger a conversation that results in a follow-up question

4. Verify:
   - Microphone does NOT start until TTS audio finishes playing
   - Device does NOT hear itself
   - Continuous conversation works correctly

---

## Component Structure Reference

```
esphome/components/voice_assistant/
├── __init__.py          # ESPHome codegen, config schema
├── voice_assistant.h    # Class definition, State enum
└── voice_assistant.cpp  # Implementation, state machine (FIX HERE)
```

### Key Types

```cpp
enum class State {
  IDLE,
  START_MICROPHONE,
  STARTING_MICROPHONE,
  WAIT_FOR_VAD,
  WAITING_FOR_VAD,
  START_PIPELINE,
  STARTING_PIPELINE,
  STREAMING_MICROPHONE,
  STOP_MICROPHONE,
  STOPPING_MICROPHONE,
  AWAITING_RESPONSE,
  STREAMING_RESPONSE,
  RESPONSE_FINISHED,  // ← Bug is in handling of this state
};

// Member variables involved:
// - media_player::MediaPlayer *media_player_{nullptr};
// - speaker::Speaker *speaker_{nullptr};
// - bool continue_conversation_{false};
```

---

## PR Checklist

- [ ] Add media player check in `RESPONSE_FINISHED` state
- [ ] Add debug log message when media player finishes
- [ ] Test with OpenAI TTS + continuous conversation
- [ ] Test with HA Cloud TTS (regression check)
- [ ] Test with internal speaker (regression check)
- [ ] Link to issue #563 in PR description

---

## References

- Voice Assistant docs: https://esphome.io/components/voice_assistant/
- MediaPlayer component: https://esphome.io/components/media_player/
- Issue #563: https://github.com/esphome/home-assistant-voice-pe/issues/563
- ESPHome voice_assistant source: https://github.com/esphome/esphome/blob/dev/esphome/components/voice_assistant/voice_assistant.cpp

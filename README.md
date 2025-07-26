

# Home Assistant Voice PE + ElevenLabs Bridge

## Overview

This project extends the Home Assistant Voice Preview Edition firmware to act as a direct bridge to ElevenLabs conversational agents. It enables real-time, two-way voice conversations between your ESP32-S3 device and ElevenLabs, bypassing intermediary protocols like Wyoming.

**Key Features:**
- Direct connection to ElevenLabs conversational agents (no Wyoming protocol)
- Real-time voice streaming and response
- ESPHome-based firmware for ESP32-S3 hardware
- Optional microphone/speaker support for flexible testing
- Persistent audio buffering in PSRAM for robust playback
- Home Assistant integration for voice assistant states and LED feedback

## How It Works

Instead of using the Wyoming protocol, this firmware connects directly to ElevenLabs via their real-time WebSocket API. The device streams audio, receives responses, and plays them back using the onboard speaker. All communication is handled natively, making the device a true ElevenLabs client.

## Getting Started

1. Flash your ESP32-S3 device with this firmware using ESPHome.
2. Configure your ElevenLabs Agent ID and (optionally) API Key in the YAML config.
3. Use the provided `test-elevenlabs.bat` script for development and testing.
4. For full setup and troubleshooting, see the [official documentation](https://voice-pe.home-assistant.io/).

## Resources

- [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/)
- [Documentation](https://voice-pe.home-assistant.io/)
- [Firmware Installer](https://esphome.github.io/home-assistant-voice-pe/)

## Notes

- This project is not compatible with Wyoming-based voice pipelines.
- All audio is streamed and played back using ElevenLabs' protocol and APIs.
- For hardware details and advanced configuration, see the YAML files and `modules/` directory.

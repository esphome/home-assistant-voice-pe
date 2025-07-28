## Project Overview
This is a fork of the Home Assistant Voice Preview Edition firmware that integrates ElevenLabs Conversational AI. The project extends the original ESPHome-based ESP32-S3 voice assistant to use ElevenLabs' real-time voice conversation API instead of the default Home Assistant pipeline.

## Architecture & Key Components

### Configuration Structure
- **`home-assistant-voice.yaml`**: Main production configuration (1800+ lines) with full Home Assistant integration, LED controls, wake word detection, and ElevenLabs integration
- **`home-assistant-voice.elevenlabs.yaml`**: Minimal test configuration for ElevenLabs component development
- **`voice-kit.yaml`**: Legacy redirect to main config for backward compatibility
- **`modules/`**: Reusable hardware modules (Grove I2C, power management)

## Critical Development Workflows
Always use web search as much as you can for *any* request, before making any changes. This is to ensure you are using the latest information and best practices. For instance, if asked to solve an SSL error for ESPHome, research the web about that error first. This is just an example - be creative about web research in general.

Never write new documentation (`.md` files or similar) unless explicitly asked to.

Never ask questions or ask for confirmation. Just do what is asked without hesitation. Make best-guess assumptions if needed, but always try to use web search first.

If asked to refactor or change something, always first read as many related files as possible to understand the context. Read the full files, not just fragments.

After making changes, always test the code by running the `.\test-elevenlabs.bat` script. This script is designed to ensure that the code works correctly with the ESP32-S3 device and that all changes are properly integrated.

### Build & Deploy Process
Always use `.\test-elevenlabs.bat` script to ensure proper workflow for testing the application, including compilation and upload to the ESP32-S3 device. This script handles:
- Compiling the ESPHome configuration
- Uploading the firmware to the device
- Running the test configuration
- Ensuring the correct COM port is used for the ESP32-S3 device
- Logging output for debugging

## Common Pitfalls
- Your development environment is on Windows, so don't try to use Linux or Unix commands. Use Powershell. However, during runtime, it is running from a Home Assistant Voice Preview Edition device, based on ESP32.
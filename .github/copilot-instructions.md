# Copilot Instructions for Home Assistant Voice PE + ElevenLabs

## Project Overview
This is a fork of the Home Assistant Voice Preview Edition firmware that integrates ElevenLabs Conversational AI. The project extends the original ESPHome-based ESP32-S3 voice assistant to use ElevenLabs' real-time voice conversation API instead of the default Home Assistant pipeline.

## Architecture & Key Components

### Configuration Structure
- **`home-assistant-voice.yaml`**: Main production configuration (1800+ lines) with full Home Assistant integration, LED controls, wake word detection, and ElevenLabs integration
- **`test-elevenlabs.yaml`**: Minimal test configuration for ElevenLabs component development - microphone/speaker optional for testing
- **`voice-kit.yaml`**: Legacy redirect to main config for backward compatibility
- **`modules/`**: Reusable hardware modules (Grove I2C, power management)

### Custom ElevenLabs Component (`esphome/components/elevenlabs_stream/`)
- **Core Implementation**: ESP-IDF WebSocket client with native WSS support for real-time audio streaming
- **Key Files**:
  - `elevenlabs_stream.cpp`: Main component with ESP WebSocket client, SSL certificate handling, signed URL authentication
  - `elevenlabs_stream.h`: Component interface with optional microphone/speaker support
  - `__init__.py`: ESPHome component schema with automation triggers/actions

### SSL/Security Implementation
- Uses ISRG Root X1 certificate for ElevenLabs API SSL validation
- Implements ElevenLabs signed URL authentication flow (HTTPS → WSS)

## Critical Development Workflows
**CRITICAL:** Always use web search as much as you can for *any* request, before asking Copilot. This is a complex project with many moving parts, and web search can provide the most up-to-date information.

### Build & Deploy Process
Always use `test-elevenlabs.bat` script to ensure proper workflow for testing the application, including compilation and upload to the ESP32-S3 device. This script handles:
- Compiling the ESPHome configuration
- Uploading the firmware to the device
- Running the test configuration
- Ensuring the correct COM port is used for the ESP32-S3 device
- Logging output for debugging

### Component Development Pattern
1. Modify C++ component files (`elevenlabs_stream.cpp/.h`)
2. Update ESPHome schema if needed (`__init__.py`)
3. Test with minimal config (`test-elevenlabs.yaml`) before full config
4. Use optional microphone/speaker for testing without hardware dependencies

### Configuration Testing Strategy
- Use `test-elevenlabs.yaml` for component development (minimal, independent)
- Use `home-assistant-voice.yaml` for full device testing
- Configs are independent - test config doesn't inherit from main config

## Project-Specific Patterns

### ESP32-S3 Hardware Integration
- Board: `esp32-s3-devkitc-1` at 240MHz
- I2S audio on specific GPIO pins (4,5,6,7)
- Grove modules use GPIO1/GPIO2 for I2C with power control on GPIO46

### ESPHome Component Architecture
- Components live in `esphome/components/` as external components
- Use ESPHome's JSON utilities for WebSocket message handling
- Implement automation triggers for voice assistant states (listening, speaking, connected, etc.)
- Optional dependencies pattern: microphone/speaker can be null for testing

## Integration Points

### ElevenLabs API Integration
- Requires Agent ID and API Key (API key optional for public agents)
- Authentication: HTTP POST to `/v1/convai/conversation/get_signed_url` → WebSocket URL
- Real-time protocol: JSON messages + binary audio frames over WebSocket

### ESPHome Integration
- Extends ESPHome with custom component using `external_components`
- Integrates with ESPHome's automation system (triggers, actions, conditions)
- Uses ESPHome's logging, JSON utilities, and component lifecycle

### Home Assistant Integration
- Voice assistant phases tracked via substitutions (idle, listening, thinking, etc.)
- LED control scripts respond to voice assistant state changes
- Integration with Home Assistant's voice pipeline architecture

## Development Guidelines

### Debugging & Logging
- Use ESPHome's logging system: `ESP_LOGD`, `ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`
- Component tag: `"elevenlabs_stream"`
- Monitor logs during development for connection issues and SSL errors

### Audio Handling
- I2S audio configuration in YAML, C++ handles streaming
- Audio frames sent as binary WebSocket messages
- Microphone/speaker components are optional for testing scenarios

## Common Pitfalls
- Always compile before upload - recent changes won't apply without compilation
- SSL certificate validation failures common in development - use bypass options
- Component dependencies: microphone/speaker optional but must be handled in code
- WebSocket connection timeout: ensure proper error handling for network issues
- ElevenLabs API authentication: requires proper signed URL flow, not direct WebSocket connection
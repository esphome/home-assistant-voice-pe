# Nanobot Voice PE firmware

This repository is a focused fork of the official ESPHome source code for the
[Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).
The upstream project remains available at
[`esphome/home-assistant-voice-pe`](https://github.com/esphome/home-assistant-voice-pe).

## Nanobot LED override

The firmware exposes a **Nanobot LED state** select entity to Home Assistant.
Selecting a state temporarily overrides the stock Assist animation:

| Option | Visual feedback | Automatic reset |
|---|---|---|
| `off` | Return control to the stock firmware | immediate |
| `thinking` | amber clockwise spinner | 90 seconds |
| `ack` | blue pulse | 1.5 seconds |
| `replying` | cyan anticlockwise spinner | 90 seconds |
| `follow_up` | violet breathing ring | 90 seconds |
| `done` | green confirmation | 1 second |
| `error` | red flash | 3 seconds |

Startup failure, provisioning, loss of the Home Assistant connection, hardware
interaction, ringing timers, mute, and native Voice Assistant error/not-ready
states retain priority over the Nanobot override. Every long-lived override has
a 90-second fail-safe and boot always starts with the override disabled.

Home Assistant can drive the state with `select.select_option`, for example:

```yaml
service: select.select_option
target:
  entity_id: select.home_assistant_voice_0a6c07_nanobot_led_state
data:
  option: ack
```

The exact entity ID is assigned by Home Assistant and may differ.

## Building and installing

The GitHub Actions **Build** workflow validates both firmware variants and
publishes build artifacts. The custom firmware intentionally disables the
official HTTP updater so an upstream release cannot silently overwrite it;
future updates are installed through ESPHome OTA or USB.

Do not flash an untested artifact onto the only production Voice PE. Download
the workflow artifact and use `home-assistant-voice-esp32s3.factory.bin` for the
initial USB installation. Keep the official web installer as the rollback path:

https://esphome.github.io/home-assistant-voice-pe/

For upstream setup and troubleshooting, see
https://voice-pe.home-assistant.io/.

# Home Assistant Voice: Preview Edition

This is the ESPHome source code of the [Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).

See [the documentation](https://voice-pe.home-assistant.io/) for set up and troubleshooting.

If you need to re-install the firmware, [use this installer](https://esphome.github.io/home-assistant-voice-pe/).

## MAX7219 directly connected to Voice PE

Use `home-assistant-voice.max7219.yaml` to build one firmware for Voice PE and
three MAX7219 matrices. The base `home-assistant-voice.yaml` stays available
for devices without the display. Voice, wake word, LED ring and sound cues
remain in the combined configuration.

Default wiring: DIN → IO41, CLK → IO40, CS → IO42, panel VCC → 5 V,
and a common GND. Confirm the actual wiring before installing; use appropriate
3.3 V to 5 V signal conversion for reliable MAX7219 operation.

```yaml
substitutions:
  name: home-assistant-voice-0ab77d
  friendly_name: Home Assistant Voice 0ab77d

packages:
  voice_pe:
    url: https://github.com/mikhailhimei/home-assistant-voice-pe
    ref: dev
    files:
      - home-assistant-voice.max7219.yaml
    refresh: always

esphome:
  name: ${name}
  name_add_mac_suffix: false
  friendly_name: ${friendly_name}

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
```

Keep any existing API encryption or other local settings. Pin assignments
and matrix count can be overridden with `max7219_din_pin`, `max7219_clk_pin`,
`max7219_cs_pin`, and `max7219_num_chips` substitutions.

The display reuses the timer and alarm data already received by Voice PE.
It shows the latest timer's countdown, freezes it during pause, and otherwise
shows `sensor.time`, with a corner dot for an enabled alarm. Enable the
Time & Date integration's time sensor in Home Assistant if it is absent.
The alarm payload does not include its scheduled time, so only alarm status
can be shown. Display brightness is adjustable in Home Assistant.

### Two matrices (16x8)

Use `home-assistant-voice.max7219-2panel.yaml` instead of
`home-assistant-voice.max7219.yaml` in the package's `files` list. It sets the
matrix count to two and selects the compact 3x5 font automatically. Wiring is
unchanged. The display has a single, one-pixel-wide colon.

Timers below 100 minutes use MM:SS. Longer timers use HH:MM with an upper-right
dot (partial minutes round up, maximum displayed value 99:59). In clock mode,
the upper-left dot indicates an enabled alarm. Without time, the display shows
`--:--`. The three-matrix version remains available.

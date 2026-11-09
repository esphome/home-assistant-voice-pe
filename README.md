# Nanobot Voice PE firmware

This repository is a focused fork of the official ESPHome source code for the
[Home Assistant Voice: Preview Edition](https://www.home-assistant.io/voice-pe/).
The upstream project remains available at
[`esphome/home-assistant-voice-pe`](https://github.com/esphome/home-assistant-voice-pe).

> [!IMPORTANT]
> Der Open-Mic-Modus ist ab Firmware `0.2.0` Bestandteil des Default-Branches `dev`; der letzte historische Firmware-Release ist [`0.2.1`](https://github.com/aschauera/home-assistant-voice-pe/releases/tag/0.2.1). Der Entwicklungsstand für das nächste gekoppelte Paar ist `1.4.2-dev`; vorgesehen sind Firmware `1.4.2` mit Tag `v1.4.2` und HA Voice Bridge `1.4.2` mit demselben Tag `v1.4.2`. Künftig verwenden Firmware und Bridge immer dieselbe SemVer und denselben `v`-Tag. Das Paar ist noch nicht veröffentlicht oder gemeinsam validiert. Historische Releases bleiben unverändert.

## Offener Dialogmodus

Die Custom Firmware unterstützt einen begrenzten halbduplexen Dialog über die
bestehende Home-Assistant-Assist-Pipeline. Ein Wake Word öffnet die Session;
nach jeder Antwort wartet das Gerät ohne neues Wake Word auf den nächsten Turn.
`Stop`, definierte Exit-Phrasen, Mute, die mittlere Taste, Fehler, zehn Minuten
Gesamtlaufzeit oder zwanzig Antworten beenden sie. Während auf eine Frage
gewartet wird, beenden zwanzig Sekunden ohne Sprachbeginn die Session. Der
Leerlauf-Timeout startet nach jeder Antwort neu und wird mit dem ersten
Sprachbeginn angehalten.

Der LED-Ablauf bleibt über Dialogzustände hinweg ruhig: ein kurzer grüner Ring
öffnet die Session; Warten und Zuhören teilen denselben violett atmenden Ring;
Verarbeitung und ein Remote-ACK bleiben langsam amberfarben; die Antwort nutzt
denselben Amber-Ton mit schnellerem Puls. Nach jeder Antwort wird wieder violett
gewartet. Ein normales Ende zeigt genau einen kurzen orangefarbenen Impuls,
Fehler blinken weiterhin rot. Der Diagnosesensor `Dialog state` behält die
genauen textuellen Zustände für Bridge-Diagnosen bei.

Eine eingebettete lokale Ansage „Einen Moment.“ kann über die ESPHome-Aktion
`dialog_remote_ack` ausgelöst werden. Die Conversation-Agent-Integration muss
diesen Hook erst nach erfolglosem Local-Intent-Routing aufrufen; die Firmware
selbst kann lokal und remote nicht unterscheiden.

Architektur, Zustandsübergänge, LED-Prioritäten, Integrationsvertrag, Grenzen
und Rollback sind in [`docs/dialogmodus.md`](docs/dialogmodus.md) dokumentiert.
Barge-in und Full-Duplex sind ausdrücklich nicht Bestandteil dieser Version.

## Nanobot LED override

The firmware exposes a **Nanobot LED state** select entity to Home Assistant.
Selecting a state temporarily overrides the stock Assist animation:

| Option | Visual feedback | Automatic reset |
|---|---|---|
| `off` | Return control to the stock firmware | immediate |
| `thinking` | slow amber breathing ring | 90 seconds |
| `ack` | same slow amber breathing ring; no separate blue indication | 1.5 seconds |
| `replying` | faster amber breathing ring | 90 seconds |
| `follow_up` | violet breathing ring | 90 seconds |
| `done` | green confirmation | 1 second |
| `error` | red flash | 3 seconds |

Startup failure, provisioning, loss of the Home Assistant connection, hardware
interaction, ringing timers, mute, and native Voice Assistant error/not-ready
states retain priority over the Nanobot override. Every long-lived override has
a 90-second fail-safe and boot always starts with the override disabled.

Home Assistant can drive the state with `select.select_option`, for example:

```yaml
action: select.select_option
target:
  entity_id: select.home_assistant_voice_0a6c07_nanobot_led_state
data:
  option: ack
```

The exact entity ID is assigned by Home Assistant and may differ.

## Building and installing

The GitHub Actions **Build** workflow validates both firmware variants with
ESPHome 2026.10.0 and publishes build artifacts. The custom firmware intentionally disables the
official HTTP updater so an upstream release cannot silently overwrite it;
future updates are installed through ESPHome OTA or USB.

Do not flash an untested artifact onto the only production Voice PE. Download
the workflow artifact and use `home-assistant-voice-esp32s3.factory.bin` for the
initial USB installation. Keep the official web installer as the rollback path:

https://esphome.github.io/home-assistant-voice-pe/

### Vorkompiliertes Release per OTA installieren

Die vollständige agentenlesbare Anleitung mit Download, Prüfsummenprüfung,
Erreichbarkeitstest, Upload, Verifikation und Rollback steht unter
[`docs/ota-flashing.md`](docs/ota-flashing.md). Die verbindliche Zuordnung von
Firmware- und Bridge-Releases steht unter
[`docs/release-pairing.md`](docs/release-pairing.md).

Kurzablauf für das Standard-Voice-PE; `VOICE_PE_IP` durch die echte Adresse
ersetzen und das Produktionsgerät nur nach ausdrücklicher Freigabe flashen:

```bash
export FIRMWARE_RELEASE="0.2.1"
export FIRMWARE_REPO="/home/nanobot/home-assistant-voice-pe"
export FIRMWARE_DIR="$HOME/Downloads/voice-pe-$FIRMWARE_RELEASE"
export VOICE_PE_IP="192.168.4.REPLACE_ME"

mkdir -p "$FIRMWARE_DIR"
cd "$FIRMWARE_DIR"
gh release download "$FIRMWARE_RELEASE" \
  --repo aschauera/home-assistant-voice-pe \
  --pattern "home-assistant-voice-$FIRMWARE_RELEASE.zip" \
  --pattern "SHA256SUMS.txt" \
  --clobber
grep "  home-assistant-voice-$FIRMWARE_RELEASE.zip$" SHA256SUMS.txt \
  | sha256sum -c -
unzip -o "home-assistant-voice-$FIRMWARE_RELEASE.zip"

python3 -m venv "$HOME/.venvs/esphome-2026.10.0"
source "$HOME/.venvs/esphome-2026.10.0/bin/activate"
python -m pip install "esphome==2026.10.0"

esphome upload "$FIRMWARE_REPO/home-assistant-voice.yaml" \
  --device "$VOICE_PE_IP" \
  --file "$FIRMWARE_DIR/home-assistant-voice-esp32s3.ota.bin" \
  --ota-platform esphome
```

Für OTA ausschließlich die Datei mit dem Suffix `.ota.bin` verwenden. Die
`.factory.bin` ist für USB-Installation oder Recovery bestimmt. Ein CLI-OTA
löscht die Gerätekonfiguration im ESPHome Device Builder nicht; ein späteres
`Install -> Wirelessly` aus dessen Weboberfläche kann die Custom Firmware aber
mit der dort gespeicherten YAML überschreiben.

For upstream setup and troubleshooting, see
https://voice-pe.home-assistant.io/.

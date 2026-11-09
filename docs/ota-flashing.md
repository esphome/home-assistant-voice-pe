# Vorkompilierte Open-Mic-Firmware per ESPHome CLI und OTA installieren

Diese Anleitung beschreibt ausschließlich das Netzwerk-OTA-Update mit dem
vorkompilierten Release-Artefakt. Sie kompiliert die Firmware nicht neu und
verändert die im Home-Assistant-ESPHome-Device-Builder gespeicherte YAML nicht.

## Sicherheits- und Kompatibilitätsgrenzen

- `v1.4.2` ist ein gekoppeltes Pre-Release und noch nicht gemeinsam auf der
  produktiven Kitchen Voice PE hardwarevalidiert. Installation nur nach
  erfolgreicher Artefaktprüfung, dokumentiertem Rollback-Pfad und ausdrücklicher
  Flash-Freigabe.
- Nur das identische Versionspaar verwenden:
  - Voice-PE-Firmware `1.4.2` mit Tag `v1.4.2` aus diesem Repository;
  - HA Voice Bridge `1.4.2` mit Tag `v1.4.2` aus dem Default-Branch `main` von
    `aschauera/NanoBotSkills`.
- Für künftige Paare müssen Firmware und Bridge dieselbe SemVer und denselben
  `v`-Tag verwenden. Historische Paare werden nicht umbenannt.
- Für OTA immer `home-assistant-voice-esp32s3.ota.bin` verwenden.
- `home-assistant-voice-esp32s3.factory.bin` ist für USB-Erstinstallation oder
  Recovery und darf nicht als OTA-Abbild verwendet werden.
- Die Variante `ha-voice-8mb-esp32s3.ota.bin` nur verwenden, wenn das konkrete
  Gerät nachweislich die Acht-Megabyte-Konfiguration benötigt. Für das normale
  Home Assistant Voice Preview Edition ist das Standardartefakt vorgesehen.
- Das einzige produktive Gerät in der Küche nicht ohne ausdrückliche Freigabe
  flashen.
- Während Upload und Neustart Strom und Netzwerk nicht unterbrechen.

## Voraussetzungen

Benötigt werden:

- Python drei mit `venv`;
- GitHub CLI `gh`, angemeldet für den Zugriff auf das Repository;
- Netzwerkzugriff vom ausführenden Rechner zum Voice PE;
- die IP-Adresse des Voice PE;
- ein lokaler Checkout dieses Repositorys auf dem Default-Branch `dev`.

Auf dem Nanobot-Pi ist der bekannte Checkout:

```text
/home/nanobot/home-assistant-voice-pe
```

Die IP-Adresse muss aus Home Assistant, ESPHome Device Builder oder UniFi
ermittelt werden. Nicht raten und nicht dauerhaft in das Repository schreiben.

## 1. Variablen setzen

`VOICE_PE_IP` vor dem Ausführen durch die tatsächliche IP-Adresse ersetzen:

```bash
export FIRMWARE_RELEASE="v1.4.2"
export FIRMWARE_REPO="/home/nanobot/home-assistant-voice-pe"
export FIRMWARE_DIR="$HOME/Downloads/voice-pe-$FIRMWARE_RELEASE"
export VOICE_PE_IP="192.168.4.REPLACE_ME"
```

Branch und Working Tree prüfen:

```bash
cd "$FIRMWARE_REPO"
git switch dev
git pull --ff-only
git status --short --branch
```

Der Working Tree muss sauber sein und `dev...origin/dev` anzeigen.

## 2. Release herunterladen

```bash
mkdir -p "$FIRMWARE_DIR"
cd "$FIRMWARE_DIR"

gh release download "$FIRMWARE_RELEASE" \
  --repo aschauera/home-assistant-voice-pe \
  --pattern "home-assistant-voice-$FIRMWARE_RELEASE.zip" \
  --pattern "SHA256SUMS.txt" \
  --clobber
```

Zuerst das unveränderte Release-Archiv prüfen:

```bash
grep "  home-assistant-voice-$FIRMWARE_RELEASE.zip$" SHA256SUMS.txt \
  | sha256sum -c -
```

Erwartete Ausgabe:

```text
home-assistant-voice-v1.4.2.zip: OK
```

Archiv entpacken:

```bash
unzip -o "home-assistant-voice-$FIRMWARE_RELEASE.zip"
ls -lh home-assistant-voice-esp32s3.ota.bin manifest.json
```

## 3. OTA-Abbild gegen das Manifest prüfen

Das folgende Kommando liest die erwartete SHA-256-Prüfsumme direkt aus dem
mitgelieferten Manifest und vergleicht sie mit der entpackten OTA-Datei:

```bash
python3 - <<'PY'
import hashlib
import json
from pathlib import Path

manifest = json.loads(Path("manifest.json").read_text(encoding="utf-8"))
ota = Path(manifest["builds"][0]["ota"]["path"])
expected = manifest["builds"][0]["ota"]["sha256"]
actual = hashlib.sha256(ota.read_bytes()).hexdigest()
print(f"Datei:    {ota}")
print(f"Erwartet: {expected}")
print(f"Geladen:  {actual}")
if actual != expected:
    raise SystemExit("FEHLER: OTA-Prüfsumme stimmt nicht")
print("OTA-Prüfsumme: OK")
PY
```

Die konkrete Prüfsumme ist releasegebunden und wird deshalb nicht aus älterer
Dokumentation übernommen. Maßgeblich sind ausschließlich `SHA256SUMS.txt` und
das `manifest.json` desselben Releases.

## 4. ESPHome CLI in einer isolierten Umgebung installieren

Die Firmware wurde mit ESPHome `2026.10.0` gebaut; dieselbe CLI-Version für den
Upload verwenden:

```bash
python3 -m venv "$HOME/.venvs/esphome-2026.10.0"
source "$HOME/.venvs/esphome-2026.10.0/bin/activate"
python -m pip install --upgrade pip
python -m pip install "esphome==2026.10.0"
esphome version
```

Erwartet wird:

```text
Version: 2026.10.0
```

## 5. Erreichbarkeit prüfen

Die ESPHome-OTA-Verbindung verwendet üblicherweise TCP-Port `3232`:

```bash
python3 - "$VOICE_PE_IP" <<'PY'
import socket
import sys

host = sys.argv[1]
with socket.create_connection((host, 3232), timeout=5):
    print(f"ESPHome OTA erreichbar: {host}:3232")
PY
```

Schlägt dies fehl, vor dem Flash Netzwerk, IP-Adresse, VLAN-/Firewall-Regeln und
Gerätestatus prüfen. Nicht auf Verdacht mit `factory.bin` fortfahren.

## 6. Vorkompilierte OTA-Firmware hochladen

```bash
cd "$FIRMWARE_DIR"
source "$HOME/.venvs/esphome-2026.10.0/bin/activate"

esphome upload "$FIRMWARE_REPO/home-assistant-voice.yaml" \
  --device "$VOICE_PE_IP" \
  --file "$FIRMWARE_DIR/home-assistant-voice-esp32s3.ota.bin" \
  --ota-platform esphome
```

Falls die bestehende Firmware entgegen der aktuellen Konfiguration ein
OTA-Passwort verlangt, dieses nur über eine Umgebungsvariable übergeben und nie
committen:

```bash
esphome upload "$FIRMWARE_REPO/home-assistant-voice.yaml" \
  --device "$VOICE_PE_IP" \
  --file "$FIRMWARE_DIR/home-assistant-voice-esp32s3.ota.bin" \
  --ota-platform esphome \
  --password "$ESPHOME_OTA_PASSWORD"
```

Der normale Open-Mic-Build definiert kein festes OTA-Passwort. Die zweite
Variante ist nur für ein bereits entsprechend geschütztes Gerät gedacht.

## 7. Neustart und Verifikation

Nach erfolgreichem Upload startet das Voice PE automatisch neu. Ein bis zwei
Minuten warten und dann prüfen:

1. Das bestehende Gerät wird in ESPHome Device Builder wieder online. Der
   CLI-Upload löscht dessen dort gespeicherte YAML nicht.
2. Home Assistant verbindet sich wieder mit demselben ESPHome-Gerät.
3. Folgende neue Entities beziehungsweise Funktionen sind vorhanden:
   - `Open dialog mode`;
   - `Dialog session active`;
   - `Dialog state`;
   - ESPHome-Aktion mit dem Suffix `_dialog_remote_ack`.
4. Die HA Voice Bridge meldet Version `1.4.1` für Integration, Manifest,
   Channel-Datei und laufenden Channel.
5. Hardwaretest in dieser Reihenfolge:
   - Wake Word;
   - grüner Aktivierungsring, violettes Warten/Zuhören und langsame Amber-Verarbeitung;
   - langsame Remote-Frage, genau ein lokales „Einen Moment.“ ohne LED-Wechsel;
   - vollständige finale Antwort;
   - schnellerer Amber-Puls während der Antwort und violettes Warten danach;
   - Folgeturn ohne neues Wake Word;
   - 20 Sekunden ohne Sprachbeginn beenden die wartende Session mit einem orangefarbenen Impuls;
   - lokaler HA-Befehl im Folgeturn;
   - `Stop` beendet die Session mit einem orangefarbenen Impuls; Fehler blinken rot.
   - WLAN-RSSI-Diagnosesensor prüfen.

Wichtig: Die ESPHome-Weboberfläche behält ihre bisherige YAML. Ein späteres
`Install -> Wirelessly` aus dieser Oberfläche kompiliert genau diese dort
hinterlegte YAML und kann die Open-Mic-Firmware wieder überschreiben. Entweder
die Dashboard-Konfiguration auf den Open-Mic-Stand bringen oder weitere
vorkompilierte Updates wieder mit `esphome upload --file` durchführen.

## 8. Fehler und Rollback

### Gerät ist weiterhin per OTA erreichbar

Eine bekannte funktionierende `*.ota.bin` kann mit demselben
`esphome upload ... --file ... --ota-platform esphome`-Befehl zurückgespielt
werden.

### Gerät ist nicht mehr per OTA erreichbar

USB-Recovery mit dem offiziellen ESPHome-Webinstaller durchführen:

```text
https://esphome.github.io/home-assistant-voice-pe/
```

Dafür das offizielle Factory-Image beziehungsweise den Webinstaller verwenden.
Ein USB-Recovery kann eine erneute Einrichtung oder Adoption erfordern.

### Bridge zurückrollen

Das beim Bridge-Deployment genannte Backup verwenden:

```bash
python3 /home/nanobot/NanoBotSkills/scripts/deploy_ha_voice_release.py \
  --rollback /home/nanobot/.nanobot/backups/ha-voice/<BACKUP>
sudo systemctl restart nanobot
```

Firmware und Bridge als gekoppeltes Paar bewerten und bei einem inkompatiblen
Stand gemeinsam zurückrollen.

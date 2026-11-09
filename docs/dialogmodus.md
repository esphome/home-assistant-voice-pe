# Offener Dialogmodus

## Ziel und Umfang

Der Dialogmodus erweitert die Home Assistant Voice Preview Edition um mehrere
aufeinanderfolgende Gesprächsrunden ohne erneutes Wake Word. Die bestehende
Home-Assistant-Assist-Pipeline bleibt vollständig erhalten: Home Assistant
führt weiterhin zuerst passende lokale Intents aus und reicht andere Anfragen
an den konfigurierten Conversation Agent weiter.

Diese Version ist bewusst **halbduplex**. Das Gerät hört zu, verarbeitet die
Anfrage, spricht die Antwort und öffnet danach das Mikrofon für den nächsten
Turn. Barge-in, Full-Duplex-Audio und eine eigene VAD sind nicht enthalten.
Home Assistants Standard-VAD bleibt unverändert aktiv.

## Aktivierung und Lebensdauer

Der Konfigurationsschalter `Open dialog mode` bestimmt, ob ein normales Wake
Word eine offene Dialogsession oder den bisherigen einzelnen Assist-Lauf
startet. Der Schalter ist in der Custom Firmware standardmäßig aktiviert.

Eine Session endet bei einem der folgenden Ereignisse:

- lokales Sleep Word `Stop`,
- erkannter STT-Text `Tschüss`, `Tschüs`, `Bye`, `Goodbye`,
  `Auf Wiedersehen` oder `Dialog beenden`,
- Druck auf die mittlere Taste,
- Hardware- oder Software-Mute,
- Verlust der Home-Assistant-Verbindung,
- Voice-Assistant-Fehler,
- zwanzig Sekunden ohne Sprachbeginn, während das Gerät auf eine Frage wartet,
- zehn Minuten Gesamtlaufzeit,
- nach der vollständig gesprochenen zwanzigsten Antwort.

Der 20-Sekunden-Leerlauf-Timer startet, sobald die Pipeline auf eine Frage
wartet, und wird nach jeder Antwort neu gestartet. Beim ersten VAD-Sprachbeginn
wird er sofort gestoppt; während Verarbeitung, Remote-ACK, TTS, Ende und Fehler
läuft er nicht. Das Zehn-Minuten-Limit gilt für die gesamte Session und wird
durch Folgeturns nicht verlängert. Der Leerlauf-Timer wird auf allen normalen
und Fehler-Exit-Pfaden gestoppt.

Der Sessionstatus wird nie über einen Neustart hinweg wiederhergestellt. Der
lokale `Stop`-Detektor ist der deterministische Exit-Pfad. Die über STT
gefundenen Exit-Phrasen sind ein zusätzlicher Best-Effort-Pfad; Home Assistant
kann den Text bereits verarbeitet haben, bevor ein Abbruch vollständig am
Server angekommen ist.

## Dialogzustände und LED-Anzeigen

Der Diagnosesensor `Dialog state` veröffentlicht zusätzlich den textuellen
Zustand. Der Binärsensor `Dialog session active` zeigt, ob eine offene Session
besteht.

| Zustand | `Dialog state` | LED-Anzeige | Bedeutung |
|---|---|---|---|
| Schlafmodus | `sleeping` | normale Idle-Anzeige | Neues Wake Word erforderlich |
| Aktivierung | `activating` | kurzer grüner Ring | Dialogsession wurde geöffnet |
| Warten | `waiting` / `follow_up` | violett atmender Ring | Wake-freier Folgeturn ist bereit |
| Zuhören | `listening` | derselbe violett atmende Ring wie beim Warten | Sprache wurde erkannt; kein separater Listening-Effekt |
| Verarbeitung | `processing` | langsam amber atmender Ring | STT, lokales Routing oder Agent laufen |
| Remote-Bestätigung | `remote_ack` | unverändert langsam amber | Remote-Agent hat übernommen; nur der Textzustand ändert sich |
| Antwort | `replying` | derselbe Amber-Ton mit schnellerem Puls | TTS-Antwort wird vorbereitet oder gesprochen |
| Beenden | `ending` | genau ein kurzer orangefarbener Impuls, danach Sleep | Session wird geschlossen |
| Fehler | `error` | rotes Blinken | Session wurde sicher abgebrochen |

Folgende Zustände behalten höhere Priorität als Dialoganzeigen:

1. Voice-Kit-Startfehler und Provisionierung,
2. fehlende WLAN- oder Home-Assistant-Verbindung,
3. physische Bedienung,
4. klingelnde Timer,
5. native Voice-Assistant-Fehler und Not-ready,
6. Hardware- oder Software-Mute.

Nach einer Antwort wechselt die Anzeige nicht in dauerhaftes Idle, sondern
zurück auf den violetten Folgeturn-Zustand, solange die Session aktiv ist. Die
letzte Antwort (Turn 20) wird vor dem normalen Ende vollständig gesprochen.

## Lokale Remote-Bestätigung

Die Firmware enthält die vorgerenderte deutsch-österreichische Ansage
`Einen Moment.`. Sie wird vollständig lokal aus
`sounds/remote_ack_einen_moment.mp3` abgespielt und benötigt weder TTS noch
einen zusätzlichen Netzwerkzugriff.

Dafür stellt ESPHome die Native-API-Aktion `dialog_remote_ack` bereit. In Home
Assistant erscheint sie unter der ESPHome-Domain mit dem tatsächlichen
Gerätenamen, beispielsweise:

```yaml
action: esphome.home_assistant_voice_0a6c07_dialog_remote_ack
```

Die Aktion spielt nur dann etwas ab, wenn eine Dialogsession aktiv und das
Gerät nicht stumm ist. Sie ist Fire-and-forget: Ein Fehler bei der Bestätigung
darf die eigentliche Remote-Anfrage nie blockieren. Der Diagnosezustand wechselt
zu `remote_ack`; die LED bleibt dabei auf der langsamen Amber-Verarbeitung.

### Routing-Vertrag

Die Firmware selbst kennt Home Assistants Entscheidung zwischen lokalem Intent
und Remote-Agent nicht. Deshalb darf ausschließlich die Conversation-Agent-
Integration diese Aktion aufrufen, und zwar erst dann, wenn Home Assistant die
lokalen Intent- und Sentence-Trigger-Prüfungen abgeschlossen und tatsächlich
den Remote-Agenten ausgewählt hat.

Für die Nanobot-Integration bedeutet das:

1. `ConversationInput.satellite_id` beziehungsweise `device_id` auswerten.
2. Beim Eintritt in `conversation.nanobot` die zum Satelliten gehörende
   ESPHome-Aktion ohne Warten auslösen.
3. Danach die Remote-Anfrage sofort und parallel fortsetzen.
4. Die bisherige gestreamte ACK-TTS deaktivieren, damit keine doppelte
   Bestätigung gesprochen wird.
5. Falls Satellit oder Aktion nicht gefunden werden, ohne ACK normal
   fortfahren.

Lokale Home-Assistant-Aktionen erreichen `conversation.nanobot` nicht und
lösen daher keine Bestätigung aus.

## Schutz gegen TTS-Selbsthören

ESPHome 2026.10.0 enthält noch den bekannten Playback-Start-Race aus
`esphome/home-assistant-voice-pe#621`. Deshalb vendort dieses Repository eine
immutable Kopie der ESPHome-Komponente `voice_assistant` aus PR `#19408`,
Commit `04caa3846f3e94b51dd2de3ca25c5d935aa992da`.

Die Komponente:

- trennt den Timeout für Playback-Start vom Stall-Watchdog,
- wartet bis zu fünfzehn Sekunden auf den Wiedergabestart,
- hält `RESPONSE_FINISHED`, solange die eigene Antwort noch abgespielt wird,
- öffnet das Mikrofon erst nach dem tatsächlichen Ende der Antwort,
- erlaubt weiterhin das Stoppen einer spät gestarteten Antwort.

Die Herkunft und der genaue Commit sind in
`custom_components/voice_assistant/README.md` dokumentiert.

## Grenzen

- Kein Barge-in: Normale Wake Words werden während einer aktiven Session
  ignoriert. `Stop` beendet die Session, startet aber keinen neuen Turn.
- Die Standard-VAD von Home Assistant bestimmt Sprachbeginn und Sprachende.
- Die Firmware kann nicht garantieren, dass STT-basierte Exit-Phrasen den
  Remote-Agenten nie erreichen; dafür wäre ein eigenes lokales Sleep-Word-
  Modell nötig.
- Das Beenden schließt das Mikrofon und den Continuous Mode. Home Assistants
  satellitenseitige Chat-Session kann die Conversation-ID noch bis zu fünf
  Minuten behalten; Home Assistant stellt derzeit keinen Reset-Hook für die
  Firmware bereit.
- Die lokale Remote-Bestätigung funktioniert erst, wenn die jeweilige
  Conversation-Agent-Integration den API-Hook aufruft.
- Eine Voice PE identifiziert nicht die sprechende Person. Alle Personen in
  Hörweite handeln mit den Rechten des in Home Assistant konfigurierten
  Voice-Benutzers.

## Build und Rollback

CI kompiliert beide Varianten mit ESPHome 2026.10.0:

- `home-assistant-voice.factory.yaml`
- `home-assistant-voice.8mb.yaml`

Die Custom Firmware deaktiviert weiterhin den offiziellen HTTP-Updater. OTA
über ESPHome und USB bleiben erhalten. Vor einem Flash sind die CI-Artefakte zu
prüfen und das offizielle Factory-Image als Rollback-Pfad bereitzuhalten.

**Diese Dokumentation erteilt keine Flash-Freigabe. Das Produktionsgerät in
der Küche darf erst nach ausdrücklicher Zustimmung aktualisiert werden.**

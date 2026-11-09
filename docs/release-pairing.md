# Release-Pairing: Voice-PE-Firmware und HA Voice Bridge

Die Open-Mic-Funktion besteht aus Firmware und Bridge. Beide Komponenten werden getrennt veröffentlicht, aber als ausdrücklich kompatibles Paar betrieben.

## Aktuelles stabiles Paar

| Komponente | Default-Branch | Release |
|---|---|---|
| Voice-PE-Firmware | `dev` | [`0.2.1`](https://github.com/aschauera/home-assistant-voice-pe/releases/tag/0.2.1) |
| HA Voice Bridge | `main` | [`v1.4.1`](https://github.com/aschauera/NanoBotSkills/releases/tag/v1.4.1) |

## Entwicklungsstand

Für den Firmware-Entwicklungsstand `0.2.2-dev` ist die HA Voice Bridge
[`v1.4.1`](https://github.com/aschauera/NanoBotSkills/releases/tag/v1.4.1) der
Kompatibilitätskandidat. Das Paar ist noch nicht gemeinsam validiert und weder
Firmware- noch Bridge-Seite wird hierdurch als neues stabiles Release
ausgewiesen. Vor einer Veröffentlichung müssen beide Komponenten gemeinsam
getestet und die Release-Notes beider Seiten direkt miteinander verlinkt werden.

## Verbindliche Regel

Jedes neue Firmware-Release muss in seinen GitHub-Release-Notes die getestete Bridge-Version direkt verlinken. Jedes neue Bridge-Release muss umgekehrt die neueste ausdrücklich kompatible Firmware-Version direkt verlinken.

Ein Verweis auf einen Branch oder auf `latest` reicht nicht, weil diese Ziele veränderlich sind. Die Release-Notizen müssen einen unveränderlichen Tag-Link enthalten.

## Veröffentlichung einer Firmware-Version

1. Kompatible Bridge-Version prüfen:

   ```bash
   gh release view v1.4.1 \
     --repo aschauera/NanoBotSkills \
     --json tagName,isPrerelease,targetCommitish,url
   ```

2. Factory- und Acht-Megabyte-Variante aus dem Zielcommit bauen.
3. Beide Artefaktarchive, Manifeste und SHA-256-Prüfsummen kontrollieren.
4. Release-Notes müssen mindestens enthalten:
   - Firmware-Version und Commit;
   - direkten Link zur kompatiblen Bridge-Version;
   - Build- und Teststatus;
   - Hinweis auf OTA-, USB- und Rollback-Pfad.
5. Ohne gemeinsame Hardwarevalidierung nur als Pre-Release veröffentlichen.
6. Nach Veröffentlichung beide Release-Seiten erneut auf Tag, Commit, Status und gegenseitige Links prüfen.

## Veröffentlichung einer Bridge-Version

Vor Veröffentlichung muss im Bridge-Repository die neueste kompatible Firmware ermittelt werden:

```bash
gh release list --repo aschauera/home-assistant-voice-pe --limit 20
gh release view 0.2.1 \
  --repo aschauera/home-assistant-voice-pe \
  --json tagName,isPrerelease,targetCommitish,url
```

Die Bridge-Release-Notes müssen den ausgegebenen Firmware-Release-Link direkt enthalten. Gibt es kein validiertes stabiles Firmware-Pendant, bleibt die Bridge ein Draft oder Pre-Release.

## Rollback

Ein Rollback wird als Paar geplant:

- Firmware auf ein bekanntes kompatibles OTA- oder Factory-Image zurücksetzen;
- Bridge über das beim Deployment erzeugte Backup oder ein früheres Release zurückrollen;
- danach Versionskonsistenz, Voice-Pipeline, ACK, finale TTS und Folgeturn erneut prüfen.

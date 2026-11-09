from __future__ import annotations

import hashlib
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CONFIG = (ROOT / "home-assistant-voice.yaml").read_text(encoding="utf-8")
WORKFLOW = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
DOC = (ROOT / "docs/dialogmodus.md").read_text(encoding="utf-8")


class CustomFirmwareContractTest(unittest.TestCase):
    def test_esphome_version_is_pinned(self) -> None:
        self.assertIn("min_version: 2026.10.0", CONFIG)
        self.assertIn("esphome-version: 2026.10.0", WORKFLOW)

    def test_dialog_safety_contract(self) -> None:
        for expected in (
            "id: dialog_session_active",
            "id: dialog_session_timeout",
            "id: dialog_idle_timeout",
            "delay: 10min",
            "delay: 20s",
            "id(dialog_turn_count) >= 20",
            "voice_assistant.start_continuous:",
            "Ignoring wake word during active dialog (barge-in disabled)",
        ):
            self.assertIn(expected, CONFIG)

    def test_idle_timeout_is_rearmed_and_stopped_once_on_every_exit_path(self) -> None:
        self.assertEqual(CONFIG.count("- script.execute: dialog_idle_timeout"), 2)
        vad_start = CONFIG.split("  on_stt_vad_start:\n", 1)[1].split(
            "  on_stt_vad_end:", 1
        )[0]
        self.assertTrue(vad_start.lstrip().startswith("- script.stop: dialog_idle_timeout"))
        for script_id in ("end_dialog_session", "fail_dialog_session"):
            script = CONFIG.split(f"  - id: {script_id}\n", 1)[1].split(
                "\n  - id: ", 1
            )[0]
            self.assertEqual(script.count("- script.stop: dialog_idle_timeout"), 1)
            self.assertEqual(script.count("- script.stop: dialog_session_timeout"), 1)
        end_after_response = CONFIG.split(
            "  - id: dialog_end_after_response\n", 1
        )[1].split("\n  - id: ", 1)[0]
        self.assertIn("- script.execute: end_dialog_session", end_after_response)
        session_timeout = CONFIG.split(
            "  - id: dialog_session_timeout\n", 1
        )[1].split("\n  - id: ", 1)[0]
        self.assertIn("- script.execute: end_dialog_session", session_timeout)

    def test_dialog_led_feedback_is_simplified(self) -> None:
        for expected in (
            "case 1:  // processing: slow amber breathing",
            "case 2:  // remote acknowledgement keeps the processing animation",
            "case 3: {  // replying: faster amber breathing",
            "case 4: {  // wake-free follow-up: violet breathing ring",
            "case 5:  // session opened: short green confirmation",
            "case 6:  // error: red flash",
            "case 7:  // normal session end: one orange blink",
            "state: 7",
        ):
            self.assertIn(expected, CONFIG)
        remote_ack = CONFIG.split("- id: play_dialog_remote_ack", 1)[1].split(
            "- id: control_leds", 1
        )[0]
        self.assertNotIn("set_nanobot_led_state", remote_ack)

    def test_nanobot_processing_state_uses_slow_amber_status_effect(self) -> None:
        controller = CONFIG.split(
            "  - id: control_leds_nanobot_state\n", 1
        )[1].split("\n  - id: ", 1)[0]
        self.assertIn('effect: "Nanobot Status"', controller)
        self.assertNotIn("Thinking Progress", controller)

    def test_wifi_audio_and_development_version_contract(self) -> None:
        self.assertIn("power_save_mode: none", CONFIG)
        self.assertIn("name: Wi-Fi RSSI", CONFIG)
        self.assertIn("entity_category: diagnostic", CONFIG)
        self.assertIn("buffer_duration: 300ms", CONFIG)
        for name in ("home-assistant-voice.factory.yaml", "home-assistant-voice.8mb.yaml"):
            self.assertIn("version: 1.4.2", (ROOT / name).read_text(encoding="utf-8"))
        for name in ("AGENTS.md", "README.md", "docs/release-pairing.md", "docs/ota-flashing.md"):
            self.assertIn("1.4.2", (ROOT / name).read_text(encoding="utf-8"))
            self.assertIn("v1.4.2", (ROOT / name).read_text(encoding="utf-8"))

    def test_dialog_led_and_diagnostic_states_are_documented(self) -> None:
        for state in (
            "sleeping",
            "activating",
            "waiting",
            "listening",
            "processing",
            "remote_ack",
            "replying",
            "follow_up",
            "ending",
            "error",
        ):
            self.assertIn(state, DOC)

    def test_remote_ack_is_local_and_guarded(self) -> None:
        sound = ROOT / "sounds/remote_ack_einen_moment.mp3"
        self.assertTrue(sound.is_file())
        self.assertGreater(sound.stat().st_size, 1_000)
        self.assertIn("action: dialog_remote_ack", CONFIG)
        self.assertIn("id(dialog_session_active)", CONFIG)
        self.assertIn('sound_file: "remote_ack_einen_moment"', CONFIG)

    def test_vendored_cpp_matches_reviewed_upstream_commit(self) -> None:
        expected = {
            "voice_assistant.cpp": "9972e4e99cd3533c02dfee34dd55c0635df041ce110dfc1509ea9f0ac14a77ef",
            "voice_assistant.h": "3712f58b5cb4bcd969026a722b13887273bec4bd24de1072b1f411ef5af4573a",
        }
        component = ROOT / "custom_components/voice_assistant"
        for name, digest in expected.items():
            actual = hashlib.sha256((component / name).read_bytes()).hexdigest()
            self.assertEqual(digest, actual, name)


if __name__ == "__main__":
    unittest.main()

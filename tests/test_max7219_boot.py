"""Check the actual ESPHome package merge keeps the speaker initialization."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import yaml


class Max7219BootTests(unittest.TestCase):
    def test_clock_package_keeps_voice_boot_and_speaker_amp(self):
        root = Path(__file__).resolve().parents[1]
        env = os.environ.copy()
        env.setdefault("PLATFORMIO_CORE_DIR", str(root / ".cache/platformio"))
        env.setdefault("XDG_CACHE_HOME", str(root / ".cache"))
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "validation.yaml"
            config.write_text(
                f"packages:\n  combined: !include {root}/home-assistant-voice.max7219.yaml\n"
                "wifi:\n  ssid: validation-only\n  password: validation-only\n"
            )
            result = subprocess.run(
                [sys.executable, "-m", "esphome", "config", str(config)],
                capture_output=True, text=True, env=env,
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        merged = yaml.load(result.stdout, Loader=yaml.BaseLoader)
        boots = merged["esphome"]["on_boot"]
        voice = next(boot for boot in boots if float(boot["priority"]) == 375)
        clock = next(boot for boot in boots if float(boot["priority"]) == -100)
        self.assertTrue(any(
            action.get("switch.turn_on", {}).get("id") == "internal_speaker_amp"
            for action in voice["then"]
        ))
        self.assertTrue(any(
            action.get("if", {}).get("condition", {}).get("switch.is_on") == {"id": "max7219_enabled"}
            for action in clock["then"]
        ))


if __name__ == "__main__":
    unittest.main()

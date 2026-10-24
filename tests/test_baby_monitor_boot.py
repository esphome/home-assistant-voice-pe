"""Validate the actual optional package alongside voice, Mute and MAX7219."""

import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]


class MonitorBootTests(unittest.TestCase):
    def test_optional_package_and_off_at_boot(self):
        if not __import__("importlib.util").util.find_spec("esphome"):
            self.skipTest("Requires ESPHome environment")
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            module = (ROOT / "modules/baby-monitor.yaml").read_text()
            module = re.sub(
                r"  - source:\n.*?(?=    components:)",
                f"  - source:\n      type: local\n      path: {ROOT}/external_components\n",
                module,
                count=1,
                flags=re.DOTALL,
            )
            (temp / "monitor.yaml").write_text(module)
            config = temp / "config.yaml"
            config.write_text(
                f"substitutions:\n  name: home-assistant-voice-0ab77d\npackages:\n  voice: !include {ROOT}/home-assistant-voice.yaml\n  monitor: !include {temp}/monitor.yaml\nwifi:\n  ssid: validation-only\n  password: validation-only\n"
            )
            env = os.environ.copy()
            env.setdefault("PLATFORMIO_CORE_DIR", "/workspace/.cache/platformio")
            env.setdefault("XDG_CACHE_HOME", "/workspace/.cache")
            result = subprocess.run(
                [sys.executable, "-m", "esphome", "config", str(config)],
                check=False,
                capture_output=True,
                text=True,
                env=env,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            merged = yaml.load(result.stdout, Loader=yaml.BaseLoader)
            switch = next(
                s for s in merged["switch"] if s.get("id") == "baby_monitor_switch"
            )
            self.assertEqual(switch["restore_mode"], "ALWAYS_OFF")
            self.assertEqual(merged["baby_monitor"]["microphone"]["channels"], ["0"])
            self.assertEqual(
                merged["baby_monitor"]["microphone"]["bits_per_sample"], "16"
            )
            self.assertEqual(len(merged["display"]), 1)
            self.assertEqual(merged["baby_monitor"]["host"], "auto")
            self.assertNotIn("upload_key", merged["baby_monitor"])
            self.assertIn("learn_server", str(merged["api"]["on_client_connected"]))
            off_actions = switch["turn_off_action"]["then"]
            self.assertEqual(off_actions[0]["script.stop"]["id"], "baby_monitor_start")
            scripts = {item["id"]: item for item in merged["script"]}
            self.assertIn("id(baby_monitor_active)", str(scripts["control_leds"]))
            self.assertIn(
                "id(baby_monitor_abort)()", str(scripts["apply_microphone_mute"])
            )


if __name__ == "__main__":
    unittest.main()

"""Firmware owns the reminder cue and a single ordered announcement queue."""
from pathlib import Path
import unittest

import yaml

ROOT = Path(__file__).resolve().parents[1]


class ReminderAnnouncementTests(unittest.TestCase):
    def setUp(self):
        self.config = yaml.load((ROOT / 'home-assistant-voice.base.yaml').read_text(), Loader=yaml.BaseLoader)
        self.script = next(item for item in self.config['script'] if item['id'] == 'play_reminder')

    def test_api_only_accepts_speech_url_and_rejects_empty_url(self):
        action = next(item for item in self.config['api']['actions'] if item['action'] == 'reminder_announce')
        self.assertEqual(action['variables'], {'url': 'string'})
        guard = action['then'][0]['if']
        self.assertEqual(guard['condition']['lambda'], 'return !url.empty();')
        self.assertEqual(guard['then'], [{'script.execute': {'id': 'play_reminder', 'url': 'return url;'}}])

    def test_embedded_chime_precedes_speech_in_same_queue(self):
        self.assertEqual(self.script['mode'], 'queued')
        self.assertEqual(self.script['max_runs'], '8')
        steps = self.script['then']
        queued = [step['media_player.enqueue'] for step in steps if 'media_player.enqueue' in step]
        self.assertEqual([step['media_url'] for step in queued],
                         ['audio-file://center_button_double_press_sound', 'return url;'])
        self.assertTrue(all(step['id'] == 'external_media_player' and step['announcement'] == 'true' for step in queued))
        self.assertIn('center_button_double_press_sound', {item['id'] for item in self.config['audio_file']})
        self.assertNotIn('media_player.stop', str(steps))
        self.assertNotIn('delay:', str(steps))

    def test_waits_for_current_playback_and_for_its_own_queue_to_finish(self):
        steps = self.script['then']
        conditions = steps[0]['wait_until']['condition']['and']
        self.assertIn({'switch.is_off': 'timer_ringing'}, conditions)
        self.assertIn({'not': {'voice_assistant.is_running': {'id': 'va'}}}, conditions)
        self.assertIn({'not': {'media_player.is_announcing': 'external_media_player'}}, conditions)
        self.assertEqual(steps[-2]['wait_until']['condition'], {'media_player.is_announcing': 'external_media_player'})
        self.assertEqual(steps[-1]['wait_until']['condition'], {'not': {'media_player.is_announcing': 'external_media_player'}})


if __name__ == '__main__':
    unittest.main()

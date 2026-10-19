"""Guard the mute state and the physical-switch release path."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import yaml


class MicrophoneMuteTests(unittest.TestCase):
    def test_hardware_release_cannot_bypass_software_mute(self):
        root = Path(__file__).resolve().parents[1]
        cfg = yaml.load((root / "home-assistant-voice.yaml").read_text(), Loader=yaml.BaseLoader)
        switch = next(item for item in cfg['switch'] if item.get('id') == 'master_mute_switch')
        hardware = next(item for item in cfg['binary_sensor'] if item.get('id') == 'hardware_mute_switch')
        sync = next(item for item in cfg['script'] if item.get('id') == 'apply_microphone_mute')
        choice = sync['then'][0]['if']
        self.assertIn({'script.execute': 'apply_microphone_mute'}, hardware['on_release'])
        self.assertFalse(any('microphone.unmute' in action for action in hardware['on_release']))
        self.assertIn({'microphone.mute': 'i2s_mics'}, choice['then'])
        self.assertIn({'voice_assistant.stop': {'id': 'va'}}, choice['then'])
        self.assertIn({'micro_wake_word.stop': {'id': 'mww'}}, choice['then'])
        self.assertEqual(choice['else'][1]['if']['condition'], {'voice_assistant.connected': {'id': 'va'}})
        source = r'''
#include <cassert>
#define id(x) x
bool software_microphone_muted=false;
struct {bool state=false;} hardware_mute_switch;
bool state() {''' + switch['lambda'] + r'''}
bool effective() {''' + choice['condition']['lambda'] + r'''}
int main() {
 for(bool software : {false,true}) for(bool hardware : {false,true}) {
  software_microphone_muted=software; hardware_mute_switch.state=hardware;
  assert(state()==(software||hardware)); assert(effective()==state());
 }
 software_microphone_muted=true; hardware_mute_switch.state=false;
 assert(effective()); // Physical release must keep software mute.
 software_microphone_muted=false; hardware_mute_switch.state=true;
 assert(effective()); // Software OFF must not bypass physical mute.
 hardware_mute_switch.state=false; assert(!effective());
}
'''
        source = '#include <initializer_list>\n' + source
        with tempfile.TemporaryDirectory() as d:
            cpp,binary=Path(d)/'mute.cpp',Path(d)/'mute'
            cpp.write_text(source)
            subprocess.run(['g++','-std=c++17',str(cpp),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)


if __name__ == '__main__':
    unittest.main()

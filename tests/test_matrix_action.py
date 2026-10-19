"""Exercise the actual automation action and pixel font without hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import yaml


class MatrixActionTests(unittest.TestCase):
    def test_text_icons_replacement_and_clear(self):
        root = Path(__file__).resolve().parents[1]
        cfg = yaml.load((root / "modules/max7219-clock.yaml").read_text(), Loader=yaml.BaseLoader)
        font = next(g for g in cfg["globals"] if g["id"] == "max7219_text_columns")["initial_value"]
        action = next(step["lambda"] for step in cfg["api"]["actions"][0]["then"] if "lambda" in step)
        delay = cfg["api"]["actions"][0]["then"][0]["delay"]
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>
#define id(x) x
#define ESP_LOGW(...) ((void)0)
uint32_t tick=1000; uint32_t millis(){return tick;}
std::vector<uint8_t> max7219_overlay_columns;
uint32_t max7219_overlay_started=0,max7219_overlay_duration=0;
bool max7219_overlay_icon=false;
auto max7219_text_columns = ''' + font + r''';
// Use long to expose mixed-type deduction bugs that int32_t triggers on Xtensa.
void show(std::string message, std::string icon, long duration) {
''' + action + r'''
}
uint32_t delay_ms(long start_delay) {
''' + delay + r'''
}
int main(){
  assert(delay_ms(-1)==0 && delay_ms(0)==0);
  assert(delay_ms(2)==2000 && delay_ms(999999)==3600000);
  show("Привет", "", 5);
  assert(max7219_overlay_columns.size()==23 && !max7219_overlay_icon);
  assert(max7219_overlay_duration==5000 && max7219_overlay_started==1000);
  auto text=max7219_overlay_columns;
  show("Привет", "none", 5); assert(max7219_overlay_columns==text && !max7219_overlay_icon);
  tick=2000; show("", "smile", 10);
  assert(max7219_overlay_icon && max7219_overlay_columns.size()==8);
  assert(max7219_overlay_duration==10000 && max7219_overlay_started==2000);
  auto icon=max7219_overlay_columns;
  show("", "unknown", 7); assert(max7219_overlay_columns==icon);
  show("", "🙂", 8); assert(max7219_overlay_columns==icon);
  show("", "", 0); assert(max7219_overlay_duration==0 && max7219_overlay_columns.empty());
  show("OK", "", 999999); assert(max7219_overlay_duration==3600000);
  show("", "", 5); assert(max7219_overlay_duration==0);
  show("STOP", "", -1); assert(max7219_overlay_duration==0);
  assert(max7219_text_columns(std::string(1000,'A')).size()==255);
}
'''
        with tempfile.TemporaryDirectory() as d:
            cpp, binary = Path(d)/"action.cpp", Path(d)/"action"
            cpp.write_text(source)
            subprocess.run(["g++", "-std=c++17", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()

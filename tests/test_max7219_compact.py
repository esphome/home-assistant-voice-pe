"""Compile and exercise the actual display lambda on a 16x8 framebuffer."""

from pathlib import Path
import subprocess
import tempfile
import unittest

import yaml


class CompactClockTests(unittest.TestCase):
    def test_two_panels_countdown_pause_and_single_colon(self):
        root = Path(__file__).resolve().parents[1]
        config = yaml.load((root / "modules/max7219-clock.yaml").read_text(), Loader=yaml.BaseLoader)
        draw = config["display"][0]["lambda"]
        source = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#define id(x) x
namespace Color { constexpr int WHITE = 1; }
uint32_t tick = 1000;
uint32_t millis() { return tick; }
bool max7219_test_active = false, dialog_timer_active = false;
bool dialog_timer_paused = false, dialog_alarm_active = false;
uint32_t dialog_timer_remaining = 215, dialog_timer_received_at = 1000;
struct Text {
  std::string state = "23:59";
  bool has_state() { return !state.empty(); }
} max7219_ha_time;
struct Matrix {
  std::array<int, 128> pixels{};
  int get_width() { return 16; }
  int get_height() { return 8; }
  void clear() { pixels.fill(0); }
  void draw_pixel_at(int x, int y, int) {
    assert(x >= 0 && x < 16 && y >= 0 && y < 8);
    pixels[y * 16 + x] = 1;
  }
};
void draw(Matrix &it) {
''' + draw + r'''
}
int main() {
  Matrix m;
  draw(m);
  auto clock = m.pixels;
  for (int y = 0; y < 8; ++y) {
    assert(m.pixels[y * 16 + 8] == (y == 3 || y == 5));
    assert(m.pixels[y * 16] == 0);
  }
  dialog_timer_active = true;
  dialog_timer_remaining = 23 * 60 + 59;
  draw(m); assert(m.pixels == clock);
  tick += 1000; draw(m); assert(m.pixels != clock);
  dialog_timer_paused = true;
  draw(m); auto paused = m.pixels;
  tick += 5000; draw(m); assert(m.pixels == paused);
  dialog_timer_paused = false;
  dialog_timer_received_at = tick;
  dialog_timer_remaining = 6000;
  draw(m); assert(m.pixels[16 + 15] == 1);
  dialog_timer_remaining = 999999;
  draw(m); // Long timers must stay in bounds and within the font table.
  dialog_timer_remaining = 0;
  draw(m); assert(m.pixels == clock);
  dialog_timer_active = false; dialog_alarm_active = true;
  draw(m); assert(m.pixels[16 + 1] == 1);
  max7219_ha_time.state = "unavailable";
  draw(m); assert(m.pixels[4 * 16 + 1] == 1);
  max7219_test_active = true;
  draw(m); for (int pixel : m.pixels) assert(pixel == 1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "display.cpp"
            binary = Path(directory) / "display"
            cpp.write_text(source)
            subprocess.run(["g++", "-std=c++17", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()

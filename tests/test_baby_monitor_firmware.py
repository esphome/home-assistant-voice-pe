"""Compile actual uplink C++ against deterministic socket/microphone interfaces."""

import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class FirmwareTests(unittest.TestCase):
    def test_ack_partial_writes_disable_congestion_and_reconnect(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            files = {
                "esphome/core/component.h": """#pragma once
namespace esphome { class Component {public: virtual void setup(){} virtual void loop(){} virtual void on_shutdown(){};}; }
""",
                "esphome/core/hal.h": """#pragma once
#include <cstdint>
namespace esphome { inline uint32_t tick=0; inline uint32_t millis(){return tick;} }
""",
                "esphome/core/log.h": "#pragma once\n#define ESP_LOGI(...) ((void)0)\n#define ESP_LOGW(...) ((void)0)\n",
                "esphome/components/microphone/microphone_source.h": """#pragma once
#include <functional>
#include <vector>
#include <cstdint>
namespace esphome::microphone { class MicrophoneSource {public:
 int starts=0,stops=0; std::function<void(const std::vector<uint8_t>&)> callback;
 template<class F> void add_data_callback(F f){callback=f;}
 void start(){starts++;} void stop(){stops++;}
}; }
""",
                "esphome/components/socket/socket.h": """#pragma once
#include <sys/socket.h>
#include <netinet/in.h>
#include <memory>
#include <string>
#include <vector>
#include <cerrno>
#include <cstring>
#include <algorithm>
namespace esphome::socket {
 inline std::vector<uint8_t> sent;
 inline std::string ack="OK\\n";
 inline size_t ack_pos=0, limit=100000;
 inline bool broken=false;
 class Socket {public:
  int setblocking(bool){return 0;}
  int connect(const sockaddr*,socklen_t){ack_pos=0; return 0;}
  ssize_t write(const void* p,size_t n){
   if(broken){errno=EPIPE; return -1;} n=std::min(n,limit);
   auto* b=static_cast<const uint8_t*>(p); sent.insert(sent.end(),b,b+n);return n;
  }
  ssize_t read(void* p,size_t n){
   if(ack_pos<ack.size()){n=std::min(n,ack.size()-ack_pos);std::memcpy(p,ack.data()+ack_pos,n);ack_pos+=n;return n;}
   errno=EAGAIN; return -1;
  }
 };
 inline std::unique_ptr<Socket> socket(int,int,int){return std::make_unique<Socket>();}
 inline socklen_t set_sockaddr(sockaddr*,socklen_t,const std::string&,uint16_t){return sizeof(sockaddr);}
}
""",
            }
            for name, content in files.items():
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            (root / "main.cpp").write_text(r"""
#include "baby_monitor.h"
#include <cassert>
using namespace esphome;
class TestMonitor : public baby_monitor::BabyMonitor {
 public: const std::string &host() const { return host_; }
};
int main(){
 microphone::MicrophoneSource mic;
 TestMonitor monitor;
 monitor.set_microphone(&mic);monitor.configure("127.0.0.1",8124,"voice",std::string(40,'k'));monitor.setup();
 monitor.learn_server("Home Assistant 2026.10", "192.0.2.1");
 assert(monitor.host()=="127.0.0.1"); // Explicit overrides always win.
 monitor.loop();assert(!monitor.is_streaming() && mic.starts==0);
 monitor.enable();monitor.loop();assert(monitor.is_streaming() && mic.starts==1);
 socket::sent.clear();socket::limit=3;
 std::vector<uint8_t> pcm{0,1,2,3,4,5,6,7};mic.callback(pcm);
 monitor.loop();monitor.loop();monitor.loop();
 assert(socket::sent==pcm); // Odd partial TCP writes must preserve sample bytes.
 monitor.disable();assert(!monitor.is_streaming());
 auto sent=socket::sent;mic.callback(pcm);monitor.loop();assert(sent==socket::sent);
 socket::limit=100000;socket::ack="NO\n";
 monitor.enable();monitor.loop();assert(!monitor.is_streaming() && mic.starts==1);
 socket::ack="OK\n";tick+=5001;monitor.loop();assert(monitor.is_streaming());
 socket::sent.clear();
 mic.callback(std::vector<uint8_t>(6400,1));mic.callback(pcm);monitor.loop();
 assert(!monitor.is_streaming() && socket::sent.empty()); // Congestion resets instead of corrupting PCM.
 tick+=5001;monitor.loop();assert(monitor.is_streaming());
 socket::broken=true;mic.callback(pcm);monitor.loop();assert(!monitor.is_streaming());
 socket::broken=false;tick+=5001;monitor.loop();assert(monitor.is_streaming());
 monitor.disable();tick+=5001;monitor.loop();assert(!monitor.is_streaming());
 microphone::MicrophoneSource auto_mic;
 TestMonitor automatic;
 automatic.set_microphone(&auto_mic);automatic.configure("auto",8124,"voice",std::string(40,'k'));automatic.setup();
 automatic.enable();automatic.loop();assert(!automatic.is_streaming() && auto_mic.starts==0);
 automatic.learn_server("ESPHome Logs", "192.0.2.2");automatic.loop();assert(automatic.host().empty());
 automatic.learn_server("Home Assistant 2026.10", "2001:db8::1");automatic.loop();assert(automatic.host().empty());
 automatic.learn_server("Home Assistant 2026.10", "192.0.2.3");automatic.loop();
 assert(automatic.host()=="192.0.2.3" && automatic.is_streaming() && auto_mic.starts==1);
 automatic.learn_server("ESPHome Logs", "192.0.2.4");assert(automatic.host()=="192.0.2.3");
 automatic.learn_server("Home Assistant 2026.10", "192.0.2.5");assert(!automatic.is_streaming());automatic.loop();
 assert(automatic.host()=="192.0.2.5" && automatic.is_streaming() && auto_mic.starts==2);
 automatic.disable();
}
""")
            component = ROOT / "external_components/baby_monitor"
            binary = root / "test"
            subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-pthread",
                    "-I" + str(root),
                    "-I" + str(component),
                    str(root / "main.cpp"),
                    str(component / "baby_monitor.cpp"),
                    "-o",
                    str(binary),
                ],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()

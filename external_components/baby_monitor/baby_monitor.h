#pragma once
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/components/microphone/microphone_source.h"
#include "esphome/components/socket/socket.h"
#include <atomic>
#include <array>
#include <mutex>

namespace esphome::baby_monitor {
class BabyMonitor : public Component {
 public:
  void set_microphone(microphone::MicrophoneSource *mic) { mic_ = mic; }
  void configure(std::string host, uint16_t port, std::string node, std::string key) {
    host_ = std::move(host); port_ = port; hello_ = node + " " + key + "\n";
  }
  void setup() override;
  void loop() override;
  void on_shutdown() override { disable(); }
  void enable();
  void disable();
  bool is_streaming() const { return ready_; }
 protected:
  void close_();
  void enqueue_(const std::vector<uint8_t> &data);
  microphone::MicrophoneSource *mic_{nullptr};
  std::unique_ptr<socket::Socket> socket_;
  std::string host_, hello_;
  uint16_t port_{};
  bool enabled_{false}, connecting_{false};
  std::atomic<bool> ready_{false}, overflow_{false};
  uint32_t last_attempt_{0}, opened_at_{0};
  size_t hello_sent_{0}, ack_size_{0};
  char ack_[3]{};
  std::mutex mutex_;
  std::array<uint8_t, 6400> buffer_{}; // At most 200 ms. No unbounded queue.
  size_t size_{0};
};
}

#ifdef USE_BABY_MONITOR_AUDIO_SOURCE
#include "monitor_audio_source.h"
#endif

#include "baby_monitor.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace esphome::baby_monitor {
static const char *const TAG = "baby_monitor";
void BabyMonitor::setup() {
  mic_->add_data_callback([this](const std::vector<uint8_t> &data) { enqueue_(data); });
}
bool BabyMonitor::provision(const std::string &key) {
  if (key.size() < 32 || key.size() > 128 ||
      key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
    ESP_LOGW(TAG, "Invalid monitor provisioning data");
    return false;
  }
  const std::string hello = node_ + " " + key + "\n";
  if (provisioned_ && hello_ == hello) return true;
  if (enabled_) close_();
  hello_ = hello;
  provisioned_ = true;
  last_attempt_ = millis() - 5000;
  return true;
}
void BabyMonitor::learn_server(const std::string &client_info, const std::string &client_address) {
  if (!automatic_host_ || client_info.rfind("Home Assistant ", 0) != 0) return;
  if (client_address.empty() || client_address.find(':') != std::string::npos) {
    ESP_LOGW(TAG, "Automatic receiver requires an IPv4 HA API peer; set baby_monitor_host to HA IPv4");
    return;
  }
  if (host_ == client_address) return;
  if (enabled_) close_();
  host_ = client_address;
  last_attempt_ = millis() - 5000;
  ESP_LOGI(TAG, "Monitor receiver learned from HA API: %s", host_.c_str());
}
void BabyMonitor::enable() {
  if (enabled_) return;
  enabled_ = true;
  last_attempt_ = millis() - 5000;
}
void BabyMonitor::disable() {
  enabled_ = false;
  close_();
}
void BabyMonitor::close_() {
  ready_ = false;  // Reject callbacks before stopping the microphone.
  mic_->stop();
  socket_.reset();
  connecting_ = false;
  std::lock_guard<std::mutex> lock(mutex_);
  size_ = 0;
}
void BabyMonitor::enqueue_(const std::vector<uint8_t> &data) {
  if (!ready_) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!ready_) return;
  // Reset the TCP stream on congestion rather than splice partial samples or replay stale sound.
  size_t count = std::min(data.size(), buffer_.size()) & ~size_t(1);
  if (size_ + count > buffer_.size()) { overflow_ = true; return; }
  std::memcpy(buffer_.data() + size_, data.data() + data.size() - count, count);
  size_ += count;
}
void BabyMonitor::loop() {
  if (!enabled_ || host_.empty() || !provisioned_) return;
  if (overflow_.exchange(false)) { close_(); return; }
  const uint32_t now = millis();
  if (!socket_) {
    if (now - last_attempt_ < 5000) return;
    last_attempt_ = opened_at_ = now;
    socket_ = socket::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (!socket_ || socket_->setblocking(false) != 0) { close_(); return; }
    sockaddr_storage address{};
    auto len = socket::set_sockaddr(reinterpret_cast<sockaddr *>(&address), sizeof(address), host_, port_);
    if (!len) { close_(); return; }
    int result = socket_->connect(reinterpret_cast<sockaddr *>(&address), len);
    if (result < 0 && errno != EINPROGRESS && errno != EWOULDBLOCK) { close_(); return; }
    connecting_ = true; hello_sent_ = ack_size_ = 0;
  }
  if (!ready_ && now - opened_at_ > 5000) { close_(); return; }
  if (connecting_) {
    // write() also handles an in-progress nonblocking connection; never block the voice loop.
    ssize_t sent = socket_->write(hello_.data() + hello_sent_, hello_.size() - hello_sent_);
    if (sent < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS || errno == ENOTCONN) return;
      close_(); return;
    }
    hello_sent_ += sent;
    if (hello_sent_ < hello_.size()) return;
    connecting_ = false;
  }
  if (!ready_) {
    auto got = socket_->read(ack_ + ack_size_, sizeof(ack_) - ack_size_);
    if (got == 0) { close_(); return; }
    if (got < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) close_(); return; }
    ack_size_ += got;
    if (ack_size_ != sizeof(ack_)) return;
    if (std::memcmp(ack_, "OK\n", 3) != 0) { close_(); return; }
    ready_ = true;
    mic_->start(); // Capture only after the receiver has accepted the device key.
    ESP_LOGI(TAG, "Audio receiver connected");
  }
  char check;
  auto got = socket_->read(&check, 1);
  if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) { close_(); return; }
  bool failed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (size_ == 0) return;
    auto sent = socket_->write(buffer_.data(), size_);
    if (sent > 0) {
      size_ -= sent;
      std::memmove(buffer_.data(), buffer_.data() + sent, size_);
    } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      failed = true;
    }
  }
  if (failed) close_();
}
}

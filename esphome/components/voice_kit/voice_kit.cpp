#include "voice_kit.h"

#include "esphome/core/defines.h"
#include "esphome/core/log.h"

namespace esphome {
namespace voice_kit {

static const char *const TAG = "voice_kit";

void VoiceKit::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Voice Kit...");

  // Reset device using the reset pin
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();
    this->reset_pin_->digital_write(true);
    delay(1);
    this->reset_pin_->digital_write(false);
  }
  // Wait for XMOS to boot...
  this->set_timeout(3000, [this]() {
    if (!this->dfu_get_version_()) {
      ESP_LOGE(TAG, "Communication with Voice Kit failed");
      this->mark_failed();
    }
  });
}

void VoiceKit::dump_config() {
  ESP_LOGCONFIG(TAG, "Voice Kit:");
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_I2C_DEVICE(this);
}

bool VoiceKit::dfu_get_version_() {
  const uint8_t DFU_CONTROLLER_SERVICER_RESID_DFU_GETVERSION = 88;
  const uint8_t version_req[] = {DFU_CONTROLLER_SERVICER_RESID,
                                 DFU_CONTROLLER_SERVICER_RESID_DFU_GETVERSION | DFU_COMMAND_READ_BIT, 4};
  uint8_t version_resp[4];

  auto error_code = this->write(version_req, sizeof(version_req));
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGW(TAG, "Request version failed");
    return false;
  }

  error_code = this->read(version_resp, sizeof(version_resp));
  if (error_code != i2c::ERROR_OK || version_resp[0] != CTRL_DONE) {
    ESP_LOGW(TAG, "Read version failed");
    return false;
  }

  ESP_LOGI(TAG, "DFU version: %u.%u.%u", version_resp[1], version_resp[2], version_resp[3]);
  return true;
}

}  // namespace voice_kit
}  // namespace esphome

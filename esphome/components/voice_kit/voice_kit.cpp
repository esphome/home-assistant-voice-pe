#include "voice_kit.h"

#include "esphome/core/application.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/log.h"

#include <cinttypes>

namespace esphome {
namespace voice_kit {

static const char *const TAG = "voice_kit";

void VoiceKit::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Voice Kit...");

  // Reset device using the reset pin
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(true);
  delay(1);
  this->reset_pin_->digital_write(false);
  // Wait for XMOS to boot...
  this->set_timeout(3000, [this]() {
    if (!this->dfu_get_version_()) {
      ESP_LOGE(TAG, "Communication with Voice Kit failed");
      this->mark_failed();
    } else if (!versions_match() && this->firmware_bin_ != nullptr && this->firmware_bin_length_) {
      ESP_LOGW(TAG, "XMOS firmware version is incorrect -- updating...");
      this->flash();
    }
  });
}

void VoiceKit::dump_config() {
  ESP_LOGCONFIG(TAG, "Voice Kit:");
  LOG_I2C_DEVICE(this);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  if (this->firmware_version_major_ || this->firmware_version_minor_ || this->firmware_version_patch_) {
    ESP_LOGCONFIG(TAG, "  XMOS firmware version: %u.%u.%u", this->firmware_version_major_,
                  this->firmware_version_minor_, this->firmware_version_patch_);
  }
}

void VoiceKit::flash() {
  if (this->firmware_bin_ == nullptr || !this->firmware_bin_length_) {
    ESP_LOGE(TAG, "Firmware invalid");
    return;
  }

  ESP_LOGI(TAG, "Starting update...");
  // #ifdef USE_OTA_STATE_CALLBACK
  //   this->state_callback_.call(ota::OTA_STARTED, 0.0f, 0);
  // #endif

  auto ota_status = this->do_ota_();

  //   switch (ota_status) {
  //     case UPDATE_OK:
  // #ifdef USE_OTA_STATE_CALLBACK
  //       this->state_callback_.call(ota::OTA_COMPLETED, 100.0f, ota_status);
  // #endif
  //       // delay(10);
  //       // App.safe_reboot();
  //       break;

  //     default:
  // #ifdef USE_OTA_STATE_CALLBACK
  //       this->state_callback_.call(ota::OTA_ERROR, 0.0f, ota_status);
  // #endif
  //       break;
  //   }
}

uint8_t VoiceKit::do_ota_() {
  uint32_t bytes_written = 0;
  uint32_t last_progress = 0;
  uint32_t update_start_time = millis();

  if (!this->dfu_get_version_()) {
    ESP_LOGE(TAG, "Initial version check failed");
    return UPDATE_READ_VERSION_ERROR;
  }

  if (!this->dfu_set_alternate_()) {
    ESP_LOGE(TAG, "Set alternate request failed");
    return UPDATE_COMMUNICATION_ERROR;
  }

  ESP_LOGV(TAG, "OTA begin");

  uint8_t dfu_dnload_req[MAX_XFER + 6] = {240, 1, 130,  // resid, cmd_id, payload length,
                                          0, 0};        // additional payload length (set below)
                                                        // followed by payload data with null terminator
  while (bytes_written < this->firmware_bin_length_) {
    if (!this->dfu_wait_for_idle_()) {
      return UPDATE_TIMEOUT;
    }

    // read a maximum of chunk_size bytes into buf. (real read size returned)
    auto bufsize = this->load_buf_(&dfu_dnload_req[5], MAX_XFER, bytes_written);
    ESP_LOGVV(TAG, "size = %u, bytes written = %u, bufsize = %u", this->firmware_bin_length_, bytes_written, bufsize);

    if (bufsize > 0 && bufsize <= MAX_XFER) {
      // write bytes to XMOS
      dfu_dnload_req[3] = (uint8_t) bufsize;
      auto error_code = this->write(dfu_dnload_req, sizeof(dfu_dnload_req) - 1);
      if (error_code != i2c::ERROR_OK) {
        ESP_LOGE(TAG, "DFU download request failed");
        return UPDATE_COMMUNICATION_ERROR;
      }
      bytes_written += bufsize;
    }

    uint32_t now = millis();
    if ((now - last_progress > 1000) or (bytes_written == this->firmware_bin_length_)) {
      last_progress = now;
      float percentage = bytes_written * 100.0f / this->firmware_bin_length_;
      ESP_LOGD(TAG, "Progress: %0.1f%%", percentage);
      // #ifdef USE_OTA_STATE_CALLBACK
      //       this->state_callback_.call(ota::OTA_IN_PROGRESS, percentage, 0);
      // #endif
    }
  }  // while

  if (!this->dfu_wait_for_idle_()) {
    return UPDATE_TIMEOUT;
  }

  memset(&dfu_dnload_req[3], 0, MAX_XFER + 2);
  // send empty download request to conclude DFU download
  auto error_code = this->write(dfu_dnload_req, sizeof(dfu_dnload_req) - 1);
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Final DFU download request failed");
    return UPDATE_COMMUNICATION_ERROR;
  }

  if (!this->dfu_wait_for_idle_()) {
    return UPDATE_TIMEOUT;
  }

  ESP_LOGI(TAG, "Done in %.0f seconds", float(millis() - update_start_time) / 1000);

  ESP_LOGI(TAG, "Rebooting XMOS SoC...");
  if (!this->dfu_reboot_()) {
    return UPDATE_COMMUNICATION_ERROR;
  }

  delay(100);  // NOLINT

  while (!this->dfu_get_version_()) {
    delay(250);  // NOLINT
    // feed watchdog and give other tasks a chance to run
    App.feed_wdt();
    yield();
  }

  ESP_LOGI(TAG, "Update complete");

  return UPDATE_OK;
}

uint32_t VoiceKit::load_buf_(uint8_t *buf, const uint8_t max_len, const uint32_t offset) {
  if (offset > this->firmware_bin_length_) {
    ESP_LOGE(TAG, "Invalid offset");
    return 0;
  }

  uint32_t buf_len = this->firmware_bin_length_ - offset;
  if (buf_len > max_len) {
    buf_len = max_len;
  }

  for (uint8_t i = 0; i < max_len; i++) {
    buf[i] = this->firmware_bin_[offset + i];
  }
  return buf_len;
}

bool VoiceKit::versions_match() {
  return this->firmware_bin_version_major_ == this->firmware_version_major_ &&
         this->firmware_bin_version_minor_ == this->firmware_version_minor_ &&
         this->firmware_bin_version_patch_ == this->firmware_version_patch_;
}

bool VoiceKit::dfu_get_status_() {
  const uint8_t status_req[] = {DFU_CONTROLLER_SERVICER_RESID,
                                DFU_CONTROLLER_SERVICER_RESID_DFU_GETSTATUS | DFU_COMMAND_READ_BIT, 6};
  uint8_t status_resp[6];

  auto error_code = this->write(status_req, sizeof(status_req));
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Request status failed");
    return false;
  }

  error_code = this->read(status_resp, sizeof(status_resp));
  if (error_code != i2c::ERROR_OK || status_resp[0] != CTRL_DONE) {
    ESP_LOGE(TAG, "Read status failed");
    return false;
  }
  this->dfu_status_next_req_delay_ = encode_uint24(status_resp[4], status_resp[3], status_resp[2]);
  this->dfu_state_ = status_resp[5];
  this->dfu_status_ = status_resp[1];
  ESP_LOGVV(TAG, "status_resp: %u %u - %ums", status_resp[1], status_resp[5], this->dfu_status_next_req_delay_);
  return true;
}

bool VoiceKit::dfu_get_version_() {
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
  this->firmware_version_major_ = version_resp[1];
  this->firmware_version_minor_ = version_resp[2];
  this->firmware_version_patch_ = version_resp[3];

  return true;
}

bool VoiceKit::dfu_reboot_() {
  const uint8_t reboot_req[] = {DFU_CONTROLLER_SERVICER_RESID, DFU_CONTROLLER_SERVICER_RESID_DFU_REBOOT, 1};

  auto error_code = this->write(reboot_req, 4);
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Reboot request failed");
    return false;
  }
  return true;
}

bool VoiceKit::dfu_set_alternate_() {
  const uint8_t setalternate_req[] = {DFU_CONTROLLER_SERVICER_RESID, DFU_CONTROLLER_SERVICER_RESID_DFU_SETALTERNATE, 1,
                                      DFU_INT_ALTERNATE_UPGRADE};  // resid, cmd_id, payload length, payload data

  auto error_code = this->write(setalternate_req, sizeof(setalternate_req));
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "SetAlternate request failed");
    return false;
  }
  return true;
}

bool VoiceKit::dfu_wait_for_idle_() {
  auto wait_for_idle_start_ms = millis();
  auto status_last_read_ms = millis();
  this->dfu_status_next_req_delay_ = 0;  // clear this, it'll be refreshed below

  do {
    if (millis() > status_last_read_ms + this->dfu_status_next_req_delay_) {
      if (!this->dfu_get_status_()) {
        return false;
      }
      status_last_read_ms = millis();
      ESP_LOGVV(TAG, "DFU state: %u, status: %u, delay: %" PRIu32, this->dfu_state_, this->dfu_status_,
                this->dfu_status_next_req_delay_);

      if ((this->dfu_state_ == DFU_INT_DFU_IDLE) || (this->dfu_state_ == DFU_INT_DFU_DNLOAD_IDLE) ||
          (this->dfu_state_ == DFU_INT_DFU_MANIFEST_WAIT_RESET)) {
        return true;
      }
    }
    // feed watchdog and give other tasks a chance to run
    App.feed_wdt();
    yield();
  } while (wait_for_idle_start_ms + DFU_TIMEOUT_MS > millis());

  ESP_LOGE(TAG, "Timeout waiting for DFU idle state");
  return false;
}

}  // namespace voice_kit
}  // namespace esphome

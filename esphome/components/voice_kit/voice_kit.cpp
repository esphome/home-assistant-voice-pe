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
    } else if (!this->versions_match_() && this->firmware_bin_is_valid_()) {
      ESP_LOGW(TAG, "Expected XMOS version: %u.%u.%u; found: %u.%u.%u. Updating...", this->firmware_bin_version_major_,
               this->firmware_bin_version_minor_, this->firmware_bin_version_patch_, this->firmware_version_major_,
               this->firmware_version_minor_, this->firmware_version_patch_);
      this->start_dfu_update();
    } else {
      this->write_pipeline_stages();
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

void VoiceKit::loop() {
  switch (this->dfu_update_status_) {
    case UPDATE_IN_PROGRESS:
    case UPDATE_REBOOT_PENDING:
    case UPDATE_VERIFY_NEW_VERSION:
      this->dfu_update_status_ = this->dfu_update_send_block_();
      break;

    case UPDATE_COMMUNICATION_ERROR:
    case UPDATE_TIMEOUT:
    case UPDATE_FAILED:
    case UPDATE_BAD_STATE:
#ifdef USE_VOICE_KIT_STATE_CALLBACK
      this->state_callback_.call(DFU_ERROR, this->bytes_written_ * 100.0f / this->firmware_bin_length_,
                                 this->dfu_update_status_);
#endif
      this->mark_failed();
      break;

    default:
      break;
  }
}

uint8_t VoiceKit::read_vnr() {
  const uint8_t vnr_req[] = {CONFIGURATION_SERVICER_RESID,
                             CONFIGURATION_SERVICER_RESID_VNR_VALUE | CONFIGURATION_COMMAND_READ_BIT, 2};
  uint8_t vnr_resp[2];

  auto error_code = this->write(vnr_req, sizeof(vnr_req));
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Request status failed");
    return 0;
  }
  error_code = this->read(vnr_resp, sizeof(vnr_resp));
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Failed to read VNR");
    return 0;
  }
  return vnr_resp[1];
}

PipelineStages VoiceKit::read_pipeline_stage(MicrophoneChannels channel) {
  uint8_t channel_register = CONFIGURATION_SERVICER_RESID_CHANNEL_0_PIPELINE_STAGE | CONFIGURATION_COMMAND_READ_BIT;
  if (channel == MICROPHONE_CHANNEL_1) {
    channel_register = CONFIGURATION_SERVICER_RESID_CHANNEL_1_PIPELINE_STAGE | CONFIGURATION_COMMAND_READ_BIT;
  }

  const uint8_t stage_req[] = {CONFIGURATION_SERVICER_RESID, channel_register, 2};

  uint8_t stage_resp[2];

  auto error_code = this->write(stage_req, sizeof(stage_req));
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Failed to read stage");
  }
  error_code = this->read(stage_resp, sizeof(stage_resp));
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Failed to read stage");
  }

  return static_cast<PipelineStages>(stage_resp[1]);
}

void VoiceKit::write_pipeline_stages() {
  // Write channel 0 stage
  uint8_t stage_set[] = {CONFIGURATION_SERVICER_RESID, CONFIGURATION_SERVICER_RESID_CHANNEL_0_PIPELINE_STAGE, 1,
                               this->channel_0_stage_};

  auto error_code = this->write(stage_set, sizeof(stage_set));
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Failed to write chanenl 0 stage");
  }

  // Write channel 1 stage
  stage_set[1] = CONFIGURATION_SERVICER_RESID_CHANNEL_1_PIPELINE_STAGE;
  stage_set[3] = this->channel_1_stage_;

  error_code = this->write(stage_set, sizeof(stage_set));
  if (error_code != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "Failed to write channel 1 stage");
  }
}

void VoiceKit::start_dfu_update() {
  if (this->firmware_bin_ == nullptr || !this->firmware_bin_length_) {
    ESP_LOGE(TAG, "Firmware invalid");
    return;
  }

  ESP_LOGI(TAG, "Starting update from %u.%u.%u...", this->firmware_version_major_, this->firmware_version_minor_,
           this->firmware_version_patch_);
#ifdef USE_VOICE_KIT_STATE_CALLBACK
  this->state_callback_.call(DFU_START, 0, UPDATE_OK);
#endif

  if (!this->dfu_set_alternate_()) {
    ESP_LOGE(TAG, "Set alternate request failed");
    this->dfu_update_status_ = UPDATE_COMMUNICATION_ERROR;
    return;
  }

  this->bytes_written_ = 0;
  this->last_progress_ = 0;
  this->last_ready_ = millis();
  this->update_start_time_ = millis();
  this->dfu_update_status_ = this->dfu_update_send_block_();
}

VoiceKitUpdaterStatus VoiceKit::dfu_update_send_block_() {
  i2c::ErrorCode error_code = i2c::NO_ERROR;
  uint8_t dfu_dnload_req[MAX_XFER + 6] = {240, 1, 130,  // resid, cmd_id, payload length,
                                          0, 0};        // additional payload length (set below)
                                                        // followed by payload data with null terminator
  if (millis() > this->last_ready_ + DFU_TIMEOUT_MS) {
    ESP_LOGE(TAG, "DFU timed out");
    return UPDATE_TIMEOUT;
  }

  if (this->bytes_written_ < this->firmware_bin_length_) {
    if (!this->dfu_check_if_ready_()) {
      return UPDATE_IN_PROGRESS;
    }

    // read a maximum of MAX_XFER bytes into buffer (real read size is returned)
    auto bufsize = this->load_buf_(&dfu_dnload_req[5], MAX_XFER, this->bytes_written_);
    ESP_LOGVV(TAG, "size = %u, bytes written = %u, bufsize = %u", this->firmware_bin_length_, this->bytes_written_,
              bufsize);

    if (bufsize > 0 && bufsize <= MAX_XFER) {
      // write bytes to XMOS
      dfu_dnload_req[3] = (uint8_t) bufsize;
      error_code = this->write(dfu_dnload_req, sizeof(dfu_dnload_req) - 1);
      if (error_code != i2c::ERROR_OK) {
        ESP_LOGE(TAG, "DFU download request failed");
        return UPDATE_COMMUNICATION_ERROR;
      }
      this->bytes_written_ += bufsize;
    }

    uint32_t now = millis();
    if ((now - this->last_progress_ > 1000) or (this->bytes_written_ == this->firmware_bin_length_)) {
      this->last_progress_ = now;
      float percentage = this->bytes_written_ * 100.0f / this->firmware_bin_length_;
      ESP_LOGD(TAG, "Progress: %0.1f%%", percentage);
#ifdef USE_VOICE_KIT_STATE_CALLBACK
      this->state_callback_.call(DFU_IN_PROGRESS, percentage, UPDATE_IN_PROGRESS);
#endif
    }
    return UPDATE_IN_PROGRESS;
  } else {  // writing the main payload is done; work out what to do next
    switch (this->dfu_update_status_) {
      case UPDATE_IN_PROGRESS:
        if (!this->dfu_check_if_ready_()) {
          return UPDATE_IN_PROGRESS;
        }
        memset(&dfu_dnload_req[3], 0, MAX_XFER + 2);
        // send empty download request to conclude DFU download
        error_code = this->write(dfu_dnload_req, sizeof(dfu_dnload_req) - 1);
        if (error_code != i2c::ERROR_OK) {
          ESP_LOGE(TAG, "Final DFU download request failed");
          return UPDATE_COMMUNICATION_ERROR;
        }
        return UPDATE_REBOOT_PENDING;

      case UPDATE_REBOOT_PENDING:
        if (!this->dfu_check_if_ready_()) {
          return UPDATE_REBOOT_PENDING;
        }
        ESP_LOGI(TAG, "Done in %.0f seconds -- rebooting XMOS SoC...",
                 float(millis() - this->update_start_time_) / 1000);
        if (!this->dfu_reboot_()) {
          return UPDATE_COMMUNICATION_ERROR;
        }
        this->last_progress_ = millis();
        return UPDATE_VERIFY_NEW_VERSION;

      case UPDATE_VERIFY_NEW_VERSION:
        if (millis() > this->last_progress_ + 200) {
          this->last_progress_ = millis();
          if (!this->dfu_get_version_()) {
            return UPDATE_VERIFY_NEW_VERSION;
          }
        } else {
          return UPDATE_VERIFY_NEW_VERSION;
        }
        if (!this->versions_match_()) {
          ESP_LOGE(TAG, "Update failed");
          return UPDATE_FAILED;
        }
        ESP_LOGI(TAG, "Update complete");
#ifdef USE_VOICE_KIT_STATE_CALLBACK
        this->state_callback_.call(DFU_COMPLETE, 100.0f, UPDATE_OK);
#endif
        this->write_pipeline_stages();
        return UPDATE_OK;

      default:
        ESP_LOGW(TAG, "Unknown state");
        return UPDATE_BAD_STATE;
    }
  }
  return UPDATE_BAD_STATE;
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

bool VoiceKit::version_read_() {
  return this->firmware_version_major_ || this->firmware_version_minor_ || this->firmware_version_patch_;
}

bool VoiceKit::versions_match_() {
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
  this->status_last_read_ms_ = millis();
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

bool VoiceKit::dfu_check_if_ready_() {
  if (millis() >= this->status_last_read_ms_ + this->dfu_status_next_req_delay_) {
    if (!this->dfu_get_status_()) {
      return false;
    }
    ESP_LOGVV(TAG, "DFU state: %u, status: %u, delay: %" PRIu32, this->dfu_state_, this->dfu_status_,
              this->dfu_status_next_req_delay_);

    if ((this->dfu_state_ == DFU_INT_DFU_IDLE) || (this->dfu_state_ == DFU_INT_DFU_DNLOAD_IDLE) ||
        (this->dfu_state_ == DFU_INT_DFU_MANIFEST_WAIT_RESET)) {
      this->last_ready_ = millis();
      return true;
    }
  }
  return false;
}

}  // namespace voice_kit
}  // namespace esphome

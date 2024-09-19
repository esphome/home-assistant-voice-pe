#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace voice_kit {

static const uint8_t REGISTER_CHANNEL_1_STAGE = 0x40;

// Configuration servicer resource IDs
//
static const uint8_t DFU_CONTROLLER_SERVICER_RESID = 240;
static const uint8_t CONFIGURATION_SERVICER_RESID = 241;
static const uint8_t CONFIGURATION_COMMAND_READ_BIT = 0x80;
static const uint8_t DFU_COMMAND_READ_BIT = 0x80;

static const uint16_t DFU_TIMEOUT_MS = 1000;
static const uint16_t MAX_XFER = 128;  // maximum number of bytes we can transfer per block

enum TransportProtocolReturnCode : uint8_t {
  CTRL_DONE = 0,
  CTRL_WAIT = 1,
  CTRL_INVALID = 3,
};

enum VoiceKitUpdaterStatus : uint8_t {
  UPDATE_OK,
  UPDATE_COMMUNICATION_ERROR,
  UPDATE_READ_VERSION_ERROR,
  UPDATE_TIMEOUT,
  UPDATE_FAILED,
  UPDATE_BAD_STATE,
  UPDATE_IN_PROGRESS,
  UPDATE_REBOOT_PENDING,
  UPDATE_VERIFY_NEW_VERSION,
};

// Configuration enums from the XMOS firmware's src/configuration/configuration_servicer.h
enum ConfCommands : uint8_t {
  CONFIGURATION_SERVICER_RESID_VNR_VALUE = 0x00,
  CONFIGURATION_SERVICER_RESID_CHANNEL_0_PIPELINE_STAGE = 0x30,
  CONFIGURATION_SERVICER_RESID_CHANNEL_1_PIPELINE_STAGE = 0x40,
};

enum PipelineStages : uint8_t {
  PIPELINE_STAGE_NONE = 0,
  PIPELINE_STAGE_AEC = 1,
  PIPELINE_STAGE_IC = 2,
  PIPELINE_STAGE_NS = 3,
  PIPELINE_STAGE_AGC = 4,
};

enum MicrophoneChannels : uint8_t {
  MICROPHONE_CHANNEL_0 = 0,
  MICROPHONE_CHANNEL_1 = 1,
};

// DFU enums from https://github.com/xmos/sln_voice/blob/develop/examples/ffva/src/dfu_int/dfu_state_machine.h

enum DfuIntAltSetting : uint8_t {
  DFU_INT_ALTERNATE_FACTORY,
  DFU_INT_ALTERNATE_UPGRADE,
};

enum DfuIntState : uint8_t {
  DFU_INT_APP_IDLE,    // unused
  DFU_INT_APP_DETACH,  // unused
  DFU_INT_DFU_IDLE,
  DFU_INT_DFU_DNLOAD_SYNC,
  DFU_INT_DFU_DNBUSY,
  DFU_INT_DFU_DNLOAD_IDLE,
  DFU_INT_DFU_MANIFEST_SYNC,
  DFU_INT_DFU_MANIFEST,
  DFU_INT_DFU_MANIFEST_WAIT_RESET,
  DFU_INT_DFU_UPLOAD_IDLE,
  DFU_INT_DFU_ERROR,
};

enum DfuIntStatus : uint8_t {
  DFU_INT_DFU_STATUS_OK,
  DFU_INT_DFU_STATUS_ERR_TARGET,
  DFU_INT_DFU_STATUS_ERR_FILE,
  DFU_INT_DFU_STATUS_ERR_WRITE,
  DFU_INT_DFU_STATUS_ERR_ERASE,
  DFU_INT_DFU_STATUS_ERR_CHECK_ERASED,
  DFU_INT_DFU_STATUS_ERR_PROG,
  DFU_INT_DFU_STATUS_ERR_VERIFY,
  DFU_INT_DFU_STATUS_ERR_ADDRESS,
  DFU_INT_DFU_STATUS_ERR_NOTDONE,
  DFU_INT_DFU_STATUS_ERR_FIRMWARE,
  DFU_INT_DFU_STATUS_ERR_VENDOR,
  DFU_INT_DFU_STATUS_ERR_USBR,
  DFU_INT_DFU_STATUS_ERR_POR,
  DFU_INT_DFU_STATUS_ERR_UNKNOWN,
  DFU_INT_DFU_STATUS_ERR_STALLEDPKT,
};

enum DfuCommands : uint8_t {
  DFU_CONTROLLER_SERVICER_RESID_DFU_DETACH = 0,
  DFU_CONTROLLER_SERVICER_RESID_DFU_DNLOAD = 1,
  DFU_CONTROLLER_SERVICER_RESID_DFU_UPLOAD = 2,
  DFU_CONTROLLER_SERVICER_RESID_DFU_GETSTATUS = 3,
  DFU_CONTROLLER_SERVICER_RESID_DFU_CLRSTATUS = 4,
  DFU_CONTROLLER_SERVICER_RESID_DFU_GETSTATE = 5,
  DFU_CONTROLLER_SERVICER_RESID_DFU_ABORT = 6,
  DFU_CONTROLLER_SERVICER_RESID_DFU_SETALTERNATE = 64,
  DFU_CONTROLLER_SERVICER_RESID_DFU_TRANSFERBLOCK = 65,
  DFU_CONTROLLER_SERVICER_RESID_DFU_GETVERSION = 88,
  DFU_CONTROLLER_SERVICER_RESID_DFU_REBOOT = 89,
};

enum DFUAutomationState {
  DFU_COMPLETE = 0,
  DFU_START,
  DFU_IN_PROGRESS,
  DFU_ERROR,
};

class VoiceKit : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  bool can_proceed() override {
    return this->is_failed() || (this->version_read_() && (this->versions_match_() || !this->firmware_bin_is_valid_()));
  }
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE - 1; }
  void loop() override;

#ifdef USE_VOICE_KIT_STATE_CALLBACK
  void add_on_state_callback(std::function<void(DFUAutomationState, float, VoiceKitUpdaterStatus)> &&callback) {
    this->state_callback_.add(std::move(callback));
  }
#endif
  void set_reset_pin(GPIOPin *reset_pin) { reset_pin_ = reset_pin; }

  void set_firmware_bin(const uint8_t *data, const uint32_t len) {
    this->firmware_bin_ = data;
    this->firmware_bin_length_ = len;
  }
  void set_firmware_version(uint8_t major, uint8_t minor, uint8_t patch) {
    this->firmware_bin_version_major_ = major;
    this->firmware_bin_version_minor_ = minor;
    this->firmware_bin_version_patch_ = patch;
  }

  void start_dfu_update();

  void set_channel_0_stage(PipelineStages channel_0_stage) { this->channel_0_stage_ = channel_0_stage; }
  void set_channel_1_stage(PipelineStages channel_1_stage) { this->channel_1_stage_ = channel_1_stage; }

  void write_pipeline_stages();
  uint8_t read_vnr();

  PipelineStages read_pipeline_stage(MicrophoneChannels channel);

 protected:
#ifdef USE_VOICE_KIT_STATE_CALLBACK
  CallbackManager<void(DFUAutomationState, float, VoiceKitUpdaterStatus)> state_callback_{};
#endif
  VoiceKitUpdaterStatus dfu_update_send_block_();
  uint32_t load_buf_(uint8_t *buf, const uint8_t max_len, const uint32_t offset);
  bool firmware_bin_is_valid_() { return this->firmware_bin_ != nullptr && this->firmware_bin_length_; }
  bool version_read_();
  bool versions_match_();

  bool dfu_get_status_();
  bool dfu_get_version_();
  bool dfu_reboot_();
  bool dfu_set_alternate_();
  bool dfu_check_if_ready_();

  PipelineStages channel_0_stage_;
  PipelineStages channel_1_stage_;

  GPIOPin *reset_pin_;

  uint8_t dfu_state_{0};
  uint8_t dfu_status_{0};
  uint32_t dfu_status_next_req_delay_{0};

  uint8_t const *firmware_bin_{nullptr};
  uint32_t firmware_bin_length_{0};
  uint8_t firmware_bin_version_major_{0};
  uint8_t firmware_bin_version_minor_{0};
  uint8_t firmware_bin_version_patch_{0};

  uint8_t firmware_version_major_{0};
  uint8_t firmware_version_minor_{0};
  uint8_t firmware_version_patch_{0};

  uint32_t bytes_written_{0};
  uint32_t last_progress_{0};
  uint32_t last_ready_{0};
  uint32_t status_last_read_ms_{0};
  uint32_t update_start_time_{0};
  VoiceKitUpdaterStatus dfu_update_status_{UPDATE_OK};
};

}  // namespace voice_kit
}  // namespace esphome

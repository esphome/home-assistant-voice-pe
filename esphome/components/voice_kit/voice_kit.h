#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace voice_kit {

// Configuration servicer resource IDs
//
static const uint8_t DFU_CONTROLLER_SERVICER_RESID = 240;
static const uint8_t CONFIGURATION_SERVICER_RESID = 241;
static const uint8_t DFU_COMMAND_READ_BIT = 0x80;
static const uint16_t DFU_TIMEOUT_MS = 1000;

enum TransportProtocolReturnCode : uint8_t {
  CTRL_DONE = 0,
  CTRL_WAIT = 1,
  CTRL_INVALID = 3,
};

class VoiceKit : public Component, public i2c::I2CDevice {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::IO; }

  void set_reset_pin(GPIOPin *reset_pin) { reset_pin_ = reset_pin; }

  bool dfu_get_version_();

 protected:
  GPIOPin *reset_pin_;
};

}  // namespace voice_kit
}  // namespace esphome
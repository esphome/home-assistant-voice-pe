#include "aic3204.h"

#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace aic3204 {

static const char *const TAG = "aic3204";

void AIC3204::setup() {
  ESP_LOGCONFIG(TAG, "Setting up AIC3204...");

  if (
      // Set register page to 0
      !this->write_byte(AIC3204_PAGE_CTRL, 0x00) ||

      // Initiate SW reset (PLL is powered off as part of reset)
      !this->write_byte(AIC3204_SW_RST, 0x01) ||

      // Program clock settings

      // Default is CODEC_CLKIN is from MCLK pin. Don't need to change this.
      /*
      // Enable PLL, MCLK is input to PLL
      !this->write_byte(AIC3204_CLK_PLL1, 0x03) ||
      // MCLK is 24.576MHz, R = 1, J = 4, D = 0, P = 3, PLL_CLK = MCLK * R * J.D / P
      !this->write_byte(AIC3204_CLK_PLL2, 0xB1) ||
      !this->write_byte(AIC3204_CLK_PLL3, 0x04) ||
      // Power up NDAC and set to 4, or could we disable PLL and just set NDAC to 3?
      !this->write_byte(AIC3204_NDAC, 0x84) ||
      // Power up MDAC and set to 4
      !this->write_byte(AIC3204_MDAC, 0x84) ||
      */
      // Power up NDAC and set to 2
      !this->write_byte(AIC3204_NDAC, 0x82) ||
      // Power up MDAC and set to 6
      !this->write_byte(AIC3204_MDAC, 0x86) ||
      // // Power up NADC and set to 1
      // !this->write_byte(AIC3204_NADC, 0x81) ||
      // // Power up MADC and set to 4
      // !this->write_byte(AIC3204_MADC, 0x84) ||
      // Program DOSR = 128
      !this->write_byte(AIC3204_DOSR, 0x80) ||
      // // Program AOSR = 128
      // !this->write_byte(AIC3204_AOSR, 0x80) ||
      // // Set Audio Interface Config: I2S, 24 bits, slave mode, DOUT always driving.
      // !this->write_byte(AIC3204_CODEC_IF, 0x20) ||
      // Set Audio Interface Config: I2S, 32 bits, slave mode, DOUT always driving.
      !this->write_byte(AIC3204_CODEC_IF, 0x30) ||

      // For I2S Firmware only, set SCLK/MFP3 pin as Audio Data In
      !this->write_byte(56, 0x02) || !this->write_byte(31, 0x01) || !this->write_byte(32, 0x01) ||

      // Program the DAC processing block to be used - PRB_P1
      !this->write_byte(AIC3204_DAC_SIG_PROC, 0x01) ||
      // Program the ADC processing block to be used - PRB_R1
      !this->write_byte(AIC3204_ADC_SIG_PROC, 0x01) ||
      // Select Page 1
      !this->write_byte(AIC3204_PAGE_CTRL, 0x01) ||
      // Enable the internal AVDD_LDO:
      !this->write_byte(AIC3204_LDO_CTRL, 0x09) ||

      //
      // Program Analog Blocks
      // ---------------------
      //
      // Disable Internal Crude AVdd in presence of external AVdd supply or before powering up internal AVdd LDO
      !this->write_byte(AIC3204_PWR_CFG, 0x08) ||
      // Enable Master Analog Power Control
      !this->write_byte(AIC3204_LDO_CTRL, 0x01) ||
      // Set Common Mode voltages: Full Chip CM to 0.9V and Output Common Mode for Headphone to 1.65V and HP powered
      // from LDOin @ 3.3V.
      !this->write_byte(AIC3204_CM_CTRL, 0x33) ||
      // Set PowerTune Modes
      // Set the Left & Right DAC PowerTune mode to PTM_P3/4. Use Class-AB driver.
      !this->write_byte(AIC3204_PLAY_CFG1, 0x00) || !this->write_byte(AIC3204_PLAY_CFG2, 0x00) ||
      // // Set the Left & Right DAC PowerTune mode to PTM_P3/4. Use Class-D driver.
      // !this->write_byte(AIC3204_PLAY_CFG1, 0xC0) ||
      // !this->write_byte(AIC3204_PLAY_CFG2, 0xC0) ||

      // // Set ADC PowerTune mode PTM_R4.
      // !this->write_byte(AIC3204_ADC_PTM, 0x00) ||
      // // Set MicPGA startup delay to 3.1ms
      // !this->write_byte(AIC3204_AN_IN_CHRG, 0x31) ||

      // Set the REF charging time to 40ms
      !this->write_byte(AIC3204_REF_STARTUP, 0x01) ||
      // HP soft stepping settings for optimal pop performance at power up
      // Rpop used is 6k with N = 6 and soft step = 20usec. This should work with 47uF coupling
      // capacitor. Can try N=5,6 or 7 time constants as well. Trade-off delay vs “pop” sound.
      !this->write_byte(AIC3204_HP_START, 0x25) ||
      // Route Left DAC to HPL
      !this->write_byte(AIC3204_HPL_ROUTE, 0x08) ||
      // Route Right DAC to HPR
      !this->write_byte(AIC3204_HPR_ROUTE, 0x08) ||
      // Route Left DAC to LOL
      !this->write_byte(0x0e, 0x08) ||
      // Route Right DAC to LOR
      !this->write_byte(0x0f, 0x08) ||
      // We are using Line input with low gain for PGA so can use 40k input R but lets stick to 20k for now.
      // // Route IN2_L to LEFT_P with 20K input impedance
      // !this->write_byte(AIC3204_LPGA_P_ROUTE, 0x20) ||
      // // Route IN2_R to LEFT_M with 20K input impedance
      // !this->write_byte(AIC3204_LPGA_N_ROUTE, 0x20) ||
      // // Route IN1_R to RIGHT_P with 20K input impedance
      // !this->write_byte(AIC3204_RPGA_P_ROUTE, 0x80) ||
      // // Route IN1_L to RIGHT_M with 20K input impedance
      // !this->write_byte(AIC3204_RPGA_N_ROUTE, 0x20) ||
      // Unmute HPL and set gain to 0dB
      !this->write_byte(AIC3204_HPL_GAIN, 0x00) ||
      // Unmute HPR and set gain to 0dB
      !this->write_byte(AIC3204_HPR_GAIN, 0x00) ||
      // Unmute LOL and set gain to 0dB
      !this->write_byte(0x12, 0x00) ||
      // Unmute LOR and set gain to 0dB
      !this->write_byte(0x13, 0x00) ||
      // Unmute Left MICPGA, Set Gain to 0dB.
      !this->write_byte(AIC3204_LPGA_VOL, 0x00) ||
      // Unmute Right MICPGA, Set Gain to 0dB.
      !this->write_byte(AIC3204_RPGA_VOL, 0x00) ||
      // // Power up HPL and HPR drivers
      // !this->write_byte(AIC3204_OP_PWR_CTRL, 0x30)
      // Power up HPL and HPR, LOL and LOR drivers
      !this->write_byte(AIC3204_OP_PWR_CTRL, 0x3C)) {
    ESP_LOGE(TAG, "AIC3204 initialization failed");
    this->mark_failed();
    return;
  }
  // Wait for 2.5 sec for soft stepping to take effect before attempting power-up
  this->set_timeout(2500, [this]() {
    if (
        //
        // Power Up DAC/ADC
        // ----------------
        //
        // Select Page 0
        !this->write_byte(AIC3204_PAGE_CTRL, 0x00) ||
        // Disable DRC
        // !this->write_byte(AIC3204_DRC_ENABLE, 0x0F) ||
        // Power up the Left and Right DAC Channels. Route Left data to Left DAC and Right data to Right DAC.
        // DAC Vol control soft step 1 step per DAC word clock.
        !this->write_byte(AIC3204_DAC_CH_SET1, 0xd4) ||
        // Power up Left and Right ADC Channels, ADC vol ctrl soft step 1 step per ADC word clock.
        !this->write_byte(AIC3204_ADC_CH_SET, 0xc0) ||
        // Unmute Left and Right DAC digital volume control
        !this->write_byte(AIC3204_DAC_CH_SET2, 0x00) ||
        // Unmute Left and Right ADC Digital Volume Control.
        !this->write_byte(AIC3204_ADC_FGA_MUTE, 0x00)) {
      ESP_LOGE(TAG, "AIC3204 power-up failed");
      this->mark_failed();
      return;
    }
  });
}

void AIC3204::dump_config() {
  ESP_LOGCONFIG(TAG, "AIC3204:");
  LOG_I2C_DEVICE(this);

  if (this->is_failed()) {
    ESP_LOGE(TAG, "Communication with AIC3204 failed");
  }
}

#ifdef USE_AUDIO_PROCESSOR
void AIC3204::set_mute_off() {
  this->is_muted = false;
  uint8_t mute_mode_byte = this->auto_mute_mode_ << 4;  // auto-mute control is bits 4-6
  mute_mode_byte |= this->is_muted ? 0x0c : 0x00;
  if (!this->write_byte(AIC3204_PAGE_CTRL, 0x00) || !this->write_byte(AIC3204_DAC_CH_SET2, mute_mode_byte)) {
    ESP_LOGE(TAG, "Unmute failed");
  }
}

void AIC3204::set_mute_on() {
  this->is_muted = true;
  uint8_t mute_mode_byte = this->auto_mute_mode_ << 4;  // auto-mute control is bits 4-6
  mute_mode_byte |= this->is_muted ? 0x0c : 0x00;
  if (!this->write_byte(AIC3204_PAGE_CTRL, 0x00) || !this->write_byte(AIC3204_DAC_CH_SET2, mute_mode_byte)) {
    ESP_LOGE(TAG, "Mute failed");
  }
}

void AIC3204::set_auto_mute_mode(optional<uint8_t> auto_mute_mode) {
  if (!auto_mute_mode.has_value()) {
    return;
  }

  auto_mute_mode = clamp<uint8_t>(auto_mute_mode.value(), 0, 7);
  ESP_LOGVV(TAG, "Setting auto_mute_mode to 0x%.2x", auto_mute_mode.value());

  this->auto_mute_mode_ = auto_mute_mode.value();
  uint8_t mute_mode_byte = this->auto_mute_mode_ << 4;  // auto-mute control is bits 4-6
  mute_mode_byte |= this->is_muted ? 0x0c : 0x00;

  if ((!this->write_byte(AIC3204_PAGE_CTRL, 0x00)) ||
      (!this->write_byte(AIC3204_DAC_CH_SET2, auto_mute_mode.value()))) {
    ESP_LOGE(TAG, "Set auto_mute_mode failed");
  }
}

void AIC3204::set_volume(optional<float> volume) {
  if (!volume.has_value()) {
    return;
  }

  const float dvc_min = -63.5;
  const float dvc_max = 24;
  const int8_t dvc_min_byte = -127;
  const int8_t dvc_max_byte = 48;

  volume = clamp<float>(volume.value(), dvc_min, dvc_max);
  int8_t volume_byte = dvc_min_byte + (2 * (volume.value() - dvc_min));
  volume_byte = clamp<int8_t>(volume_byte, dvc_min_byte, dvc_max_byte);

  ESP_LOGVV(TAG, "Setting volume to 0x%.2x", volume_byte & 0xFF);

  if ((!this->write_byte(AIC3204_PAGE_CTRL, 0x00)) || (!this->write_byte(AIC3204_DACL_VOL_D, volume_byte)) ||
      (!this->write_byte(AIC3204_DACR_VOL_D, volume_byte))) {
    ESP_LOGE(TAG, "Set volume failed");
  }
}
#endif

}  // namespace aic3204
}  // namespace esphome

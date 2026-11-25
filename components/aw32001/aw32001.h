#pragma once

#include "esphome/components/i2c/i2c.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include <span>

namespace esphome::aw32001 {

// AW32001E datasheet V1.2, "Register List"
static constexpr uint8_t AW32001_REG_POWER_ON = 0x01;  // EN_HIZ[4], CEB[3]
static constexpr uint8_t AW32001_REG_ICHG = 0x02;      // REG_RST[7], WD_TMR_RST[6], ICHG[5:0]
static constexpr uint8_t AW32001_REG_MAIN = 0x06;      // FET_DIS[5]
static constexpr uint8_t AW32001_REG_STATUS = 0x08;
static constexpr uint8_t AW32001_REG_FAULT = 0x09;
static constexpr uint8_t AW32001_REG_CHIP_ID = 0x0A;
static constexpr uint8_t AW32001_CHIP_ID = 0x49;
static constexpr uint8_t AW32001_STATUS_WATCHDOG = 0x80;
static constexpr uint8_t AW32001_ICHG_RESET_BITS = 0xC0;
static constexpr uint8_t AW32001_ICHG_WATCHDOG_RESET = 0x40;

/// One setting from the configuration: these bits of the register get this value
struct AW32001RegisterUpdate {
  uint8_t reg;
  uint8_t mask;
  uint8_t value;
};

#ifdef USE_TEXT_SENSOR
/// Publish CHG_STAT (REG08[4:3])
void publish_charge_status(text_sensor::TextSensor *sensor, uint8_t status);
/// Publish the active faults of REG08 and REG09 as a comma separated list, or "none"
void publish_faults(text_sensor::TextSensor *sensor, uint8_t status, uint8_t fault);
#endif

class AW32001Component final : public PollingComponent, public i2c::I2CDevice {
 public:
  /// `config` is a PROGMEM table from code generation, written in order
  AW32001Component(const AW32001RegisterUpdate *config, size_t count, bool auto_kick)
      : config_(config, count), auto_kick_(auto_kick) {}

  void setup() override;
  void update() override;
  void dump_config() override;

  bool kick_watchdog();
  /// Write the bits of `value` under `mask` into `reg`
  bool update_bits(uint8_t reg, uint8_t mask, uint8_t value);
#ifdef USE_AW32001_STATUS_PUBLISHER
  HUB_PUBLISHER(status, uint8_t, uint8_t)
#endif
#ifdef USE_AW32001_SWITCH_PUBLISHER
  HUB_PUBLISHER(switches, uint8_t, uint8_t)
#endif

 protected:
  bool apply_config_();

  std::span<const AW32001RegisterUpdate> config_;
  bool auto_kick_;
  bool config_pending_{true};  // the first poll, from setup(), writes it
};

}  // namespace esphome::aw32001

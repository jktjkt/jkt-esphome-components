#pragma once

// BQ27220 technical reference manual: https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf

#include "esphome/components/i2c/i2c.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/progmem.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif

#include "bq27220_regs.h"

#include <span>

namespace esphome::bq27220_full {

PROGMEM_STRING_TABLE(SecurityAccessStrings, "reserved", "full_access", "unsealed", "sealed");

/// A flag of a status word, by its name in the manual; code generation puts the table in flash
struct FlagDef {
  char name[10];
  StatusWord word;
  uint8_t bit;
};

#ifdef USE_BINARY_SENSOR
inline void publish_flag(binary_sensor::BinarySensor *sensor, const Reading &reading, StatusWord word, uint8_t bit) {
  if (reading.status_valid(word))
    sensor->publish_state(reading.flag(word, bit));
}
#endif

#ifdef USE_TEXT_SENSOR
inline void publish_security_access(text_sensor::TextSensor *sensor, const Reading &reading) {
  if (reading.status_valid(StatusWord::STATUS_WORD_OPERATION))
    sensor->publish_state(
        SecurityAccessStrings::get_progmem_str(security_access(reading.status(StatusWord::STATUS_WORD_OPERATION))));
}
/// Publish the names of the set flags of the words that were read, separated by spaces, or "none"
void publish_active_flags(text_sensor::TextSensor *sensor, const Reading &reading, std::span<const FlagDef> flags);
#endif

class BQ27220FullComponent final : public PollingComponent, public i2c::I2CDevice {
#ifdef USE_TEXT_SENSOR
  SUB_TEXT_SENSOR(device_number)
  SUB_TEXT_SENSOR(firmware_version)
  SUB_TEXT_SENSOR(hardware_version)
#endif

 public:
  void setup() override;
  void update() override;
  void dump_config() override;

  void set_design_capacity(uint16_t capacity) { this->design_capacity_ = capacity; }
  void set_taper_current(uint16_t current) { this->taper_current_ = current; }
  void set_reset_on_boot(bool reset) { this->reset_on_boot_ = reset; }
  /// Read the GaugingStatus subcommand at every poll, for the entities that show its flags
  void set_read_gauging_status(bool read) { this->read_gauging_status_ = read; }
  /// RESET subcommand: the gauge restarts and its RAM settings return to the defaults
  bool reset_gauge();
#ifdef USE_BQ27220_FULL_STATUS_PUBLISHER
  HUB_PUBLISHER(status, const Reading &)
  HUB_PUBLISHER(dump, const char *)
#endif

 protected:
  /// Run a MAC subcommand and copy `length` bytes of its answer. Waits, so it is for setup() only.
  bool read_mac_(uint16_t subcommand, uint8_t *data, uint8_t length);
  /// Write the configured data memory into the gauge's RAM if it differs. Waits, so it is for setup() only.
  void apply_data_memory_();
  bool send_control_(uint16_t word);
  bool read_security_(SecurityAccess *access);
  bool unseal_();
  bool enter_full_access_();
  void restore_security_(SecurityAccess initial);
  bool wait_init_complete_();
  bool wait_config_update_(bool active);
  bool read_data_memory_(uint16_t address, uint16_t *value);
  bool write_data_memory_(uint16_t address, uint16_t value);
  void on_gauging_status_();
  void publish_();

  Reading reading_;
  uint16_t design_capacity_{0};
  uint16_t taper_current_{0};
  bool reset_on_boot_{false};
  bool read_gauging_status_{false};
  bool gauging_failure_logged_{false};
};

}  // namespace esphome::bq27220_full

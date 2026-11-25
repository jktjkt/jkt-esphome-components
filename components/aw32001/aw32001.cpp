#include "aw32001.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/core/progmem.h"
#endif

namespace esphome::aw32001 {

ESPHOME_LOG_TAG(TAG, "aw32001");

#ifdef USE_TEXT_SENSOR
PROGMEM_STRING_TABLE(AW32001ChargeStates, "not_charging", "pre_charge", "fast_charge", "charge_done");
// REG08[7], then REG09[5] down to REG09[0]
PROGMEM_STRING_TABLE(AW32001Faults, "watchdog", "input", "thermal_shutdown", "battery_ovp", "safety_timer", "ntc_hot",
                     "ntc_cold");

void publish_charge_status(text_sensor::TextSensor *sensor, uint8_t status) {
  sensor->publish_state(AW32001ChargeStates::get_progmem_str((status >> 3) & 0x03));
}

void publish_faults(text_sensor::TextSensor *sensor, uint8_t status, uint8_t fault) {
  // One bit per name, the first name in bit 6; REG09[7:6] are the shipping deglitch setting, not faults
  const uint8_t active = ((status & AW32001_STATUS_WATCHDOG) >> 1) | (fault & 0x3F);
  if (active == 0) {
    sensor->publish_state("none");
    return;
  }
  // All the names with a separator after each fit
  char text[AW32001Faults::BLOB_SIZE + AW32001Faults::COUNT];
  size_t pos = 0;
  for (uint8_t i = 0; i < AW32001Faults::COUNT; i++) {
    if ((active & (0x40 >> i)) == 0)
      continue;
    if (pos != 0)
      pos = buf_append_str(text, sizeof(text), pos, ", ");
    pos = buf_append_str_p(text, sizeof(text), pos, reinterpret_cast<ESPHOME_PGM_P>(AW32001Faults::get_progmem_str(i)));
  }
  sensor->publish_state(text, pos);
}
#endif

static AW32001RegisterUpdate read_update(const AW32001RegisterUpdate &entry) {
  AW32001RegisterUpdate update;
  progmem_memcpy(&update, &entry, sizeof(update));
  return update;
}

bool AW32001Component::update_bits(uint8_t reg, uint8_t mask, uint8_t value) {
  // setup() rejected the device at this address, so nothing is written into it
  if (this->is_failed())
    return false;
  if (this->update_register(reg, mask, value))
    return true;
  ESP_LOGW(TAG, "Failed to update register 0x%02X", reg);
  return false;
}

bool AW32001Component::kick_watchdog() {
  // Sets WD_TMR_RST and keeps ICHG; the reset of all registers beside it stays clear
  return this->update_bits(AW32001_REG_ICHG, AW32001_ICHG_RESET_BITS, AW32001_ICHG_WATCHDOG_RESET);
}

bool AW32001Component::apply_config_() {
  // Stops at the first failure: the charge enable comes last and must not follow a setting that was not written
  for (const auto &entry : this->config_) {
    const AW32001RegisterUpdate update = read_update(entry);
    if (!this->update_bits(update.reg, update.mask, update.value))
      return false;
  }
  return true;
}

void AW32001Component::setup() {
  // Another device answering at this address must not get the configuration
  if (!this->check_chip_id(this, AW32001_REG_CHIP_ID, AW32001_CHIP_ID))
    return;
  // The first poll writes the configuration and gives the entities a state now
  this->update();
}

void AW32001Component::dump_config() {
  ESP_LOGCONFIG(TAG,
                "AW32001:\n"
                "  Watchdog kick: %s",
                YESNO(this->auto_kick_));
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
  for (const auto &entry : this->config_) {
    const AW32001RegisterUpdate update = read_update(entry);
    ESP_LOGCONFIG(TAG, "  REG%02X mask 0x%02X value 0x%02X", update.reg, update.mask, update.value);
  }
}

void AW32001Component::update() {
  if (this->config_pending_)
    this->config_pending_ = !this->apply_config_();
  uint8_t status;
  uint8_t fault;
  // Reading REG09 clears its latched faults, so each poll reads it once and publishes all of it
  bool read_ok = this->read_byte(AW32001_REG_STATUS, &status) && this->read_byte(AW32001_REG_FAULT, &fault);
  if (read_ok) {
    ESP_LOGV(TAG, "REG08 status 0x%02X, REG09 fault 0x%02X", status, fault);
    if ((status & AW32001_STATUS_WATCHDOG) != 0) {
      // Most registers are back at their power-on values
      ESP_LOGW(TAG, "Watchdog expired, applying the configuration again");
      this->config_pending_ = !this->apply_config_();
    }
  }
  // Also after a failed read, so that one failed transfer does not let the watchdog expire
  const bool kicked = !this->auto_kick_ || this->kick_watchdog();

  if (read_ok) {
#ifdef USE_AW32001_STATUS_PUBLISHER
    if (this->status_publisher_ != nullptr)
      this->status_publisher_(status, fault);
#endif
#ifdef USE_AW32001_SWITCH_PUBLISHER
    uint8_t power_on;
    uint8_t main_control;
    if (this->switches_publisher_ != nullptr) {
      read_ok = this->read_byte(AW32001_REG_POWER_ON, &power_on) && this->read_byte(AW32001_REG_MAIN, &main_control);
      if (read_ok)
        this->switches_publisher_(power_on, main_control);
    }
#endif
  }
  // The warning stays while a setting or the watchdog kick could not be written
  this->status_comm_result(read_ok && kicked && !this->config_pending_);
}

}  // namespace esphome::aw32001

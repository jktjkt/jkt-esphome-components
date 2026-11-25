#include "bq27220_full.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::bq27220_full {

ESPHOME_LOG_TAG(TAG, "bq27220_full");

// MAC subcommands (SLUUBD4A 2.2): written to 0x00, the answer is echoed at 0x3E and follows at 0x40
static constexpr uint8_t REG_MAC_CONTROL = 0x00;
static constexpr uint8_t REG_MAC_DATA = 0x3E;  // also takes a data memory address
static constexpr uint8_t REG_MAC_DATA_BLOCK = 0x40;
static constexpr uint8_t REG_MAC_DATA_SUM = 0x60;  // checksum, then length
static constexpr uint16_t SUBCOMMAND_DEVICE_NUMBER = 0x0001;
static constexpr uint16_t SUBCOMMAND_FW_VERSION = 0x0002;
static constexpr uint16_t SUBCOMMAND_HW_VERSION = 0x0003;
static constexpr uint16_t SUBCOMMAND_SEALED = 0x0030;
static constexpr uint16_t SUBCOMMAND_RESET = 0x0041;
static constexpr uint16_t SUBCOMMAND_GAUGING_STATUS = 0x0056;
// Data memory update, SLUUBD4A chapter 6. The RAM copy of the parameters is at 0x92xx
static constexpr uint16_t SUBCOMMAND_ENTER_CFG_UPDATE = 0x0090;
static constexpr uint16_t SUBCOMMAND_EXIT_CFG_UPDATE_REINIT = 0x0091;
static constexpr uint16_t SUBCOMMAND_EXIT_CFG_UPDATE = 0x0092;
static constexpr uint16_t KEY_UNSEAL_1 = 0x0414;
static constexpr uint16_t KEY_UNSEAL_2 = 0x3672;
static constexpr uint16_t KEY_FULL_ACCESS = 0xFFFF;
static constexpr uint16_t DEVICE_NUMBER = 0x0220;
static constexpr uint16_t DATA_MEMORY_TAPER_CURRENT = 0x9201;
static constexpr uint16_t DATA_MEMORY_LEARNED_CAPACITY = 0x929D;  // CEDV profile 1, learned full charge capacity
static constexpr uint16_t DATA_MEMORY_DESIGN_CAPACITY = 0x929F;
// The length written with the checksum counts the address, the data and itself with the checksum
static constexpr uint8_t DATA_MEMORY_WRITE_LENGTH = 6;
static constexpr uint16_t OPERATION_INITCOMP = 0x0020;
static constexpr uint16_t OPERATION_CFGUPDATE = 0x0400;
static constexpr uint32_t CFG_UPDATE_WAIT_MS = 1100;  // wait after entering the mode, SLUUBD4A 8.1.4
static constexpr uint32_t CFG_UPDATE_TIMEOUT_MS = 3000;
static constexpr uint32_t POLL_MS = 50;
// Timings taken from the Flipper Zero BQ27220 driver, which works on real gauges
static constexpr uint32_t CONTROL_PAUSE_MS = 5;
static constexpr uint32_t RESET_SETTLE_MS = 300;
static constexpr uint32_t RESET_TIMEOUT_MS = 4000;
static constexpr uint32_t CONFIG_APPLY_WAIT_MS = 2000;
static constexpr uint32_t FULL_ACCESS_TIMEOUT_MS = 500;
static constexpr uint32_t VERIFY_TIMEOUT_MS = 3000;
static constexpr uint32_t VERIFY_POLL_MS = 100;
static constexpr uint8_t FW_VERSION_LENGTH = 11;  // ddDDvvVVbbBBTTzzZZRREE
static constexpr uint8_t HW_VERSION_LENGTH = 2;
static constexpr uint8_t MAC_MAX_LENGTH = FW_VERSION_LENGTH;
static constexpr uint32_t MAC_WAIT_MS = 10;
// The first transfer after a power-up has failed once on the Nesso N1 and worked on the next boot
static constexpr uint8_t DEVICE_NUMBER_ATTEMPTS = 3;
// After a reset the gauge needs a moment to answer again, M5Unified waits 200 ms
static constexpr uint32_t RESET_WAIT_MS = 300;
static constexpr uint8_t DEVICE_NUMBER_ATTEMPTS_AFTER_RESET = 10;
static constexpr uint32_t DEVICE_NUMBER_RETRY_MS = 50;
// Every flag name with a space before it fits
static constexpr size_t ACTIVE_FLAGS_SIZE = 256;

static constexpr uint32_t GAUGING_STATUS_TIMEOUT_ID = 0;

#ifdef USE_TEXT_SENSOR
void publish_active_flags(text_sensor::TextSensor *sensor, const Reading &reading, std::span<const FlagDef> flags) {
  char text[ACTIVE_FLAGS_SIZE];
  size_t pos = 0;
  for (const FlagDef &flag : flags) {
    const auto word = static_cast<StatusWord>(progmem_read_byte(reinterpret_cast<const uint8_t *>(&flag.word)));
    if (!reading.status_valid(word) || !reading.flag(word, progmem_read_byte(&flag.bit)))
      continue;
    if (pos != 0)
      pos = buf_append_str(text, sizeof(text), pos, " ");
    pos = buf_append_str_p(text, sizeof(text), pos, flag.name);
  }
  if (pos == 0) {
    sensor->publish_state(ESPHOME_F("none"));
  } else {
    sensor->publish_state(text, pos);
  }
}
#endif

bool BQ27220FullComponent::read_mac_(uint16_t subcommand, uint8_t *data, uint8_t length) {
  if (!this->write_byte_16_le(REG_MAC_CONTROL, subcommand)) {
    ESP_LOGW(TAG, "Writing subcommand 0x%04X failed", subcommand);
    return false;
  }
  delay(MAC_WAIT_MS);
  // The answer is echoed at 0x3E and follows from 0x40
  uint8_t buffer[2 + MAC_MAX_LENGTH];
  if (this->read_bytes(REG_MAC_DATA, buffer, 2 + length)) {
    const uint16_t echo = encode_uint16(buffer[1], buffer[0]);
    if (echo != subcommand) {
      ESP_LOGW(TAG, "Subcommand 0x%04X was answered with 0x%04X", subcommand, echo);
      return false;
    }
    memcpy(data, buffer + 2, length);
    return true;
  }
  // Read the data alone, as the ESPHome bq27220 component does. The echo cannot be checked then.
  ESP_LOGW(TAG, "Reading the echo of subcommand 0x%04X failed, trying the data alone", subcommand);
  if (this->read_bytes(REG_MAC_DATA_BLOCK, data, length))
    return true;
  ESP_LOGW(TAG, "Reading the answer of subcommand 0x%04X failed", subcommand);
  return false;
}

void BQ27220FullComponent::setup() {
  uint8_t attempts = DEVICE_NUMBER_ATTEMPTS;
  if (this->reset_on_boot_) {
    ESP_LOGW(TAG, "reset_on_boot is set, the gauge is reset at every boot. Remove it after one boot");
    this->reset_gauge();
    delay(RESET_WAIT_MS);
    attempts = DEVICE_NUMBER_ATTEMPTS_AFTER_RESET;
  }
  uint8_t data[MAC_MAX_LENGTH];
  bool read_ok = false;
  for (uint8_t attempt = 0; attempt < attempts && !read_ok; attempt++) {
    if (attempt > 0)
      delay(DEVICE_NUMBER_RETRY_MS);
    read_ok = this->read_mac_(SUBCOMMAND_DEVICE_NUMBER, data, 2);
  }
  if (!this->setup_check(read_ok, LOG_STR(ESP_LOG_MSG_COMM_FAIL)))
    return;
  const uint16_t device_number = encode_uint16(data[1], data[0]);
  if (device_number != DEVICE_NUMBER) {
    ESP_LOGE(TAG, "Unexpected device number 0x%04X (expected 0x%04X)", device_number, DEVICE_NUMBER);
    this->mark_failed(LOG_STR("Unexpected device number"));
    return;
  }
#ifdef USE_TEXT_SENSOR
  PUBLISH_STATE(this->device_number_text_sensor_, ESPHOME_F("0x0220"));
#endif
  char text[format_hex_pretty_size(MAC_MAX_LENGTH)];
  if (this->read_mac_(SUBCOMMAND_FW_VERSION, data, FW_VERSION_LENGTH)) {
    format_hex_pretty_to(text, data, FW_VERSION_LENGTH, ' ');
    ESP_LOGV(TAG, "Firmware version bytes: %s", text);
#ifdef USE_TEXT_SENSOR
    PUBLISH_STATE(this->firmware_version_text_sensor_, text);
#endif
  } else {
    ESP_LOGW(TAG, "Failed to read the firmware version");
  }
  if (this->read_mac_(SUBCOMMAND_HW_VERSION, data, HW_VERSION_LENGTH)) {
#ifdef USE_TEXT_SENSOR
    format_hex_pretty_to(text, data, HW_VERSION_LENGTH, ' ');
    PUBLISH_STATE(this->hardware_version_text_sensor_, text);
#endif
  } else {
    ESP_LOGW(TAG, "Failed to read the hardware version");
  }

  if (this->design_capacity_ != 0 || this->taper_current_ != 0)
    this->apply_data_memory_();
  this->update();
}

void BQ27220FullComponent::update() {
  uint8_t valid = 0;
  for (size_t i = 0; i < BLOCKS.size(); i++) {
    if (this->read_bytes(BLOCKS[i].start, this->reading_.registers.data() + BLOCKS[i].start, BLOCKS[i].length))
      valid |= 1 << i;
  }
  this->reading_.valid = valid;
  const bool ok = valid == (1 << BLOCKS.size()) - 1;
  if (ok) {
    ESP_LOGV(TAG, "ControlStatus 0x%04X BatteryStatus 0x%04X OperationStatus 0x%04X",
             this->reading_.word(REG_CONTROL_STATUS), this->reading_.word(REG_BATTERY_STATUS),
             this->reading_.word(REG_OPERATION_STATUS));
  }
  this->status_comm_result(ok, LOG_STR("Failed to read BQ27220 registers"));

  if (this->read_gauging_status_ && this->write_byte_16_le(REG_MAC_CONTROL, SUBCOMMAND_GAUGING_STATUS)) {
    this->set_timeout(GAUGING_STATUS_TIMEOUT_ID, MAC_WAIT_MS, [this]() { this->on_gauging_status_(); });
    return;
  }
  this->publish_();
}

void BQ27220FullComponent::on_gauging_status_() {
  uint8_t answer[4];
  if (this->read_bytes(REG_MAC_DATA, answer, sizeof(answer)) &&
      encode_uint16(answer[1], answer[0]) == SUBCOMMAND_GAUGING_STATUS) {
    this->reading_.gauging_status = encode_uint16(answer[3], answer[2]);
    this->reading_.valid |= Reading::GAUGING_STATUS_VALID;
    ESP_LOGV(TAG, "GaugingStatus 0x%04X", this->reading_.gauging_status);
  } else if (!this->gauging_failure_logged_) {
    this->gauging_failure_logged_ = true;
    ESP_LOGW(TAG, "GaugingStatus subcommand failed, its flags are not published");
  }
  this->publish_();
}

void BQ27220FullComponent::publish_() {
#ifdef USE_BQ27220_FULL_STATUS_PUBLISHER
  if (this->status_publisher_ != nullptr)
    this->status_publisher_(this->reading_);
#endif
}

bool BQ27220FullComponent::send_control_(uint16_t word) {
  const bool ok = this->write_byte_16_le(REG_MAC_CONTROL, word);
  // Two control writes in a row fail unless they are about 2.5 ms apart (found by the Flipper Zero driver, which
  // uses 5 ms)
  delay(CONTROL_PAUSE_MS);
  return ok;
}

bool BQ27220FullComponent::read_security_(SecurityAccess *access) {
  uint16_t status;
  if (!this->read_byte_16_le(REG_OPERATION_STATUS, &status))
    return false;
  *access = security_access(status);
  return true;
}

bool BQ27220FullComponent::unseal_() { return this->send_control_(KEY_UNSEAL_1) && this->send_control_(KEY_UNSEAL_2); }

bool BQ27220FullComponent::enter_full_access_() {
  SecurityAccess access;
  if (!this->read_security_(&access))
    return false;
  if (access == SecurityAccess::SECURITY_ACCESS_FULL_ACCESS)
    return true;
  if (access == SecurityAccess::SECURITY_ACCESS_SEALED && !this->unseal_())
    return false;
  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    if (!(this->send_control_(KEY_FULL_ACCESS) && this->send_control_(KEY_FULL_ACCESS)))
      return false;
    for (uint32_t waited = 0; waited <= FULL_ACCESS_TIMEOUT_MS; waited += POLL_MS) {
      if (this->read_security_(&access) && access == SecurityAccess::SECURITY_ACCESS_FULL_ACCESS)
        return true;
      delay(POLL_MS);
    }
  }
  ESP_LOGW(TAG, "The gauge did not enter full access, security mode is %s",
           LOG_STR_ARG(SecurityAccessStrings::get_log_str(access)));
  return false;
}

void BQ27220FullComponent::restore_security_(SecurityAccess initial) {
  SecurityAccess now;
  if (!this->read_security_(&now) || now == initial || initial == SecurityAccess::SECURITY_ACCESS_FULL_ACCESS)
    return;
  // Back to the mode the gauge was in: sealed, or sealed and unsealed again. There is no step down from full access.
  this->send_control_(SUBCOMMAND_SEALED);
  delay(MAC_WAIT_MS);
  if (initial != SecurityAccess::SECURITY_ACCESS_SEALED)
    this->unseal_();
  if (this->read_security_(&now)) {
    ESP_LOGV(TAG, "Security mode is back to %s", LOG_STR_ARG(SecurityAccessStrings::get_log_str(now)));
  }
}

bool BQ27220FullComponent::wait_init_complete_() {
  delay(RESET_SETTLE_MS);
  for (uint32_t waited = 0; waited < RESET_TIMEOUT_MS; waited += POLL_MS) {
    uint16_t status;
    if (this->read_byte_16_le(REG_OPERATION_STATUS, &status) && (status & OPERATION_INITCOMP) &&
        !(status & OPERATION_CFGUPDATE))
      return true;
    delay(POLL_MS);
    App.feed_wdt();
  }
  return false;
}

bool BQ27220FullComponent::wait_config_update_(bool active) {
  for (uint32_t waited = 0; waited < CFG_UPDATE_TIMEOUT_MS; waited += POLL_MS) {
    uint16_t status;
    if (this->read_byte_16_le(REG_OPERATION_STATUS, &status) && ((status & OPERATION_CFGUPDATE) != 0) == active)
      return true;
    delay(POLL_MS);
    App.feed_wdt();
  }
  return false;
}

bool BQ27220FullComponent::read_data_memory_(uint16_t address, uint16_t *value) {
  // Select the block of the address, then the word is at 0x40, big-endian
  if (!this->write_byte_16_le(REG_MAC_DATA, address))
    return false;
  delay(MAC_WAIT_MS);
  return this->read_byte_16(REG_MAC_DATA_BLOCK, value);
}

bool BQ27220FullComponent::write_data_memory_(uint16_t address, uint16_t value) {
  // As the manual's short example (4.6) and the Flipper Zero driver do it: the little-endian address and the
  // big-endian value to 0x3E, then the complement of their sum and the length to 0x60. Nothing is read in between.
  uint8_t message[4];
  put_be16(put_le16(message, address), value);
  const uint8_t commit[2] = {static_cast<uint8_t>(~sum_bytes(message, sizeof(message))), DATA_MEMORY_WRITE_LENGTH};
  if (!this->write_bytes(REG_MAC_DATA, message, sizeof(message)))
    return false;
  delay(MAC_WAIT_MS);
  if (!this->write_bytes(REG_MAC_DATA_SUM, commit, sizeof(commit)))
    return false;
  delay(MAC_WAIT_MS);
  ESP_LOGV(TAG, "Data memory 0x%04X set to %u, checksum 0x%02X", address, value, commit[0]);
  return true;
}

void BQ27220FullComponent::apply_data_memory_() {
  SecurityAccess initial;
  if (!this->read_security_(&initial) || (initial == SecurityAccess::SECURITY_ACCESS_SEALED && !this->unseal_())) {
    ESP_LOGW(TAG, "Setting the data memory failed, the gauge could not be unsealed");
    return;
  }

  // The words to write, and whether any of them differs from what the gauge holds now. The words are read outside the
  // config update mode, as the Flipper Zero driver does for its checks. A reset puts every RAM setting back to its
  // default, so once one word differs, all configured ones are written.
  struct Word {
    uint16_t address;
    uint16_t value;
  };
  StaticVector<Word, 3> words;
  bool differs = false;

  if (this->design_capacity_ != 0) {
    uint16_t current;
    if (!this->read_byte_16_le(REG_DESIGN_CAPACITY, &current)) {
      ESP_LOGW(TAG, "Failed to read the design capacity");
    } else if (current != this->design_capacity_) {
      ESP_LOGI(TAG, "Design capacity is %u mAh, setting %u mAh", current, this->design_capacity_);
      differs = true;
    }
    // The learned full charge capacity two bytes below it is what the gauge starts from
    words.push_back({DATA_MEMORY_DESIGN_CAPACITY, this->design_capacity_});
    words.push_back({DATA_MEMORY_LEARNED_CAPACITY, this->design_capacity_});
  }
  if (this->taper_current_ != 0) {
    uint16_t taper;
    if (!this->read_data_memory_(DATA_MEMORY_TAPER_CURRENT, &taper)) {
      ESP_LOGW(TAG, "Failed to read the taper current");
    } else if (taper != this->taper_current_) {
      ESP_LOGI(TAG, "Taper current is %u mA, setting %u mA", taper, this->taper_current_);
      differs = true;
    }
    words.push_back({DATA_MEMORY_TAPER_CURRENT, this->taper_current_});
  }
  if (!differs) {
    ESP_LOGV(TAG, "The data memory settings are already as configured");
    this->restore_security_(initial);
    return;
  }

  // The Flipper Zero driver resets the gauge before writing a configuration: "Don't forget to reset chip before
  // writing new config". The reset also returns the gauge to the unsealed mode.
  this->reset_gauge();
  bool ok = this->wait_init_complete_() && this->enter_full_access_();
  if (!ok)
    ESP_LOGW(TAG, "The gauge did not come back from the reset in full access");
  bool entered = false;
  if (ok) {
    // Through 0x3E, as the manual's example and the Flipper Zero driver do
    entered = this->write_byte_16_le(REG_MAC_DATA, SUBCOMMAND_ENTER_CFG_UPDATE);
    delay(CFG_UPDATE_WAIT_MS);
    ok = entered && this->wait_config_update_(true);
  }
  bool written = false;
  if (ok) {
    written = true;
    for (const Word &word : words)
      written = this->write_data_memory_(word.address, word.value) && written;
    ok = written;
  }
  if (entered) {
    // Re-initialise only if something was written, and give the gauge its time to apply it: the Flipper Zero driver
    // waits 2 s here because "DM read returns garbage" before
    this->send_control_(written ? SUBCOMMAND_EXIT_CFG_UPDATE_REINIT : SUBCOMMAND_EXIT_CFG_UPDATE);
    for (uint32_t waited = 0; waited < CONFIG_APPLY_WAIT_MS; waited += POLL_MS) {
      delay(POLL_MS);
      App.feed_wdt();
    }
    if (!this->wait_config_update_(false))
      ESP_LOGW(TAG, "The gauge did not leave the config update mode");
  }
  this->restore_security_(initial);
  if (!ok) {
    ESP_LOGW(TAG, "Setting the data memory failed");
    return;
  }

  // Read everything back, the design capacity through its standard command, which may take a moment
  if (this->design_capacity_ != 0) {
    uint16_t reported = 0;
    bool done = false;
    for (uint32_t waited = 0; waited <= VERIFY_TIMEOUT_MS && !done; waited += VERIFY_POLL_MS) {
      done = this->read_byte_16_le(REG_DESIGN_CAPACITY, &reported) && reported == this->design_capacity_;
      if (done) {
        ESP_LOGI(TAG, "Design capacity set to %u mAh, the gauge reports it after %" PRIu32 " ms", reported, waited);
      } else {
        delay(VERIFY_POLL_MS);
        App.feed_wdt();
      }
    }
    if (!done)
      ESP_LOGW(TAG, "The gauge still reports a design capacity of %u mAh after the write", reported);
  }
  if (this->taper_current_ != 0) {
    uint16_t taper = 0;
    if (this->read_data_memory_(DATA_MEMORY_TAPER_CURRENT, &taper) && taper == this->taper_current_) {
      ESP_LOGI(TAG, "Taper current set to %u mA", taper);
    } else {
      ESP_LOGW(TAG, "Taper current reads %u mA after the write, not %u mA", taper, this->taper_current_);
    }
  }
}

bool BQ27220FullComponent::reset_gauge() {
  if (!this->write_byte_16_le(REG_MAC_CONTROL, SUBCOMMAND_RESET)) {
    ESP_LOGW(TAG, "Sending the gauge reset failed");
    return false;
  }
  ESP_LOGI(TAG, "Gauge reset sent, its RAM settings return to the defaults");
  return true;
}

void BQ27220FullComponent::dump_config() {
  ESP_LOGCONFIG(TAG,
                "BQ27220 fuel gauge:\n"
                "  Design capacity: %u mAh\n"
                "  Taper current: %u mA\n"
                "  Reset on boot: %s\n"
                "  GaugingStatus polled: %s",
                this->design_capacity_, this->taper_current_, YESNO(this->reset_on_boot_),
                YESNO(this->read_gauging_status_));
  LOG_I2C_DEVICE(this);
  LOG_UPDATE_INTERVAL(this);
#ifdef USE_TEXT_SENSOR
  LOG_TEXT_SENSOR("  ", "Device Number", this->device_number_text_sensor_);
  LOG_TEXT_SENSOR("  ", "Firmware Version", this->firmware_version_text_sensor_);
  LOG_TEXT_SENSOR("  ", "Hardware Version", this->hardware_version_text_sensor_);
#endif
#ifdef USE_BQ27220_FULL_STATUS_PUBLISHER
  if (this->dump_publisher_ != nullptr)
    this->dump_publisher_(TAG);
#endif
}

}  // namespace esphome::bq27220_full

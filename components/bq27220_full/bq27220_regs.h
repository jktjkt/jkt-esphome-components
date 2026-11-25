#pragma once

// The standard command registers of the BQ27220 fuel gauge and how the entities decode them, see the technical
// reference manual SLUUBD4A, https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf
// Standard library only and constexpr, so that tests/components/bq27220_full/regs_test.cpp checks it with
// static_assert.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace esphome::bq27220_full {

/// How a sensor turns its register into a value
enum class Conversion : uint8_t {
  CONVERSION_RAW,
  CONVERSION_MILLI,
  CONVERSION_MILLI_SIGNED,
  CONVERSION_MILLI_SENTINEL,  // 0xFFFF means "not applicable"
  CONVERSION_DECI_KELVIN,     // to degrees Celsius
  CONVERSION_SENTINEL,        // 0xFFFF means "not applicable"
  CONVERSION_LOW3,
  CONVERSION_SIGNED,
};

/// The words that hold the flags
enum class StatusWord : uint8_t {
  STATUS_WORD_CONTROL,    // CONTROL_STATUS(), 0x00
  STATUS_WORD_BATTERY,    // BatteryStatus(), 0x0A
  STATUS_WORD_OPERATION,  // OperationStatus(), 0x3A
  STATUS_WORD_GAUGING,    // GaugingStatus() MAC subcommand 0x0056
};

/// OperationStatus()[SEC1:0]
enum class SecurityAccess : uint8_t {
  SECURITY_ACCESS_RESERVED,
  SECURITY_ACCESS_FULL_ACCESS,
  SECURITY_ACCESS_UNSEALED,
  SECURITY_ACCESS_SEALED,
};

// SLUUBD4A table 2-1: the standard commands are 16-bit little-endian registers. Each poll reads them in these blocks
// of consecutive documented addresses, one I2C transfer each.
struct Block {
  uint8_t start;
  uint8_t length;
};
static constexpr std::array<Block, 4> BLOCKS{{
    {0x00, 14},  // CONTROL_STATUS, AtRate, AtRateTimeToEmpty, Temperature, Voltage, BatteryStatus, Current
    {0x10, 22},  // RemainingCapacity ... AveragePower
    {0x28, 16},  // InternalTemperature ... BTPChargeSet
    {0x3A, 4},   // OperationStatus, DesignCapacity
}};
/// Every register of the blocks fits below this
static constexpr size_t REGISTER_SPACE = 0x3E;

static constexpr uint8_t REG_CONTROL_STATUS = 0x00;
static constexpr uint8_t REG_BATTERY_STATUS = 0x0A;
static constexpr uint8_t REG_OPERATION_STATUS = 0x3A;
static constexpr uint8_t REG_DESIGN_CAPACITY = 0x3C;

// After DEVICE_NUMBER or FW_VERSION the next read of 0x00 returns this once instead of CONTROL_STATUS
static constexpr uint16_t CONTROL_STATUS_MOVED = 0xFFA5;
// "not applicable" in several registers
static constexpr uint16_t SENTINEL = 0xFFFF;

/// The block the register is in, or -1
constexpr int block_of(uint8_t reg) {
  for (size_t i = 0; i < BLOCKS.size(); i++) {
    if (reg >= BLOCKS[i].start && reg < BLOCKS[i].start + BLOCKS[i].length)
      return int(i);
  }
  return -1;
}

/// The value of a sensor from its register, NaN for "not applicable"
constexpr float convert(uint16_t raw, Conversion conversion) {
  constexpr float not_applicable = std::numeric_limits<float>::quiet_NaN();
  switch (conversion) {
    case Conversion::CONVERSION_MILLI:
      return raw * 0.001f;
    case Conversion::CONVERSION_MILLI_SIGNED:
      return static_cast<int16_t>(raw) * 0.001f;
    case Conversion::CONVERSION_MILLI_SENTINEL:
      return raw == SENTINEL ? not_applicable : raw * 0.001f;
    case Conversion::CONVERSION_DECI_KELVIN:
      return raw * 0.1f - 273.15f;
    case Conversion::CONVERSION_SENTINEL:
      return raw == SENTINEL ? not_applicable : float(raw);
    case Conversion::CONVERSION_LOW3:
      return float(raw & 0x07);
    case Conversion::CONVERSION_SIGNED:
      return float(static_cast<int16_t>(raw));
    case Conversion::CONVERSION_RAW:
      break;
  }
  return float(raw);
}

constexpr SecurityAccess security_access(uint16_t operation_status) {
  return static_cast<SecurityAccess>((operation_status >> 1) & 0x03);
}

/// What one poll read: the register blocks, GaugingStatus, and which of them were read
struct Reading {
  /// Bit i: BLOCKS[i] was read
  static constexpr uint8_t GAUGING_STATUS_VALID = 1 << BLOCKS.size();

  std::array<uint8_t, REGISTER_SPACE> registers{};
  uint16_t gauging_status{0};
  uint8_t valid{0};

  constexpr uint16_t word(uint8_t reg) const { return uint16_t(this->registers[reg] | this->registers[reg + 1] << 8); }
  constexpr bool register_valid(uint8_t reg) const {
    const int block = block_of(reg);
    return block >= 0 && (this->valid >> block & 1) &&
           (reg != REG_CONTROL_STATUS || this->word(reg) != CONTROL_STATUS_MOVED);
  }
  /// The value of a sensor, NaN when its register was not read
  constexpr float value(uint8_t reg, Conversion conversion) const {
    return this->register_valid(reg) ? convert(this->word(reg), conversion) : std::numeric_limits<float>::quiet_NaN();
  }
  constexpr bool status_valid(StatusWord word) const {
    switch (word) {
      case StatusWord::STATUS_WORD_CONTROL:
        return this->register_valid(REG_CONTROL_STATUS);
      case StatusWord::STATUS_WORD_BATTERY:
        return this->register_valid(REG_BATTERY_STATUS);
      case StatusWord::STATUS_WORD_OPERATION:
        return this->register_valid(REG_OPERATION_STATUS);
      case StatusWord::STATUS_WORD_GAUGING:
        break;
    }
    return this->valid & GAUGING_STATUS_VALID;
  }
  constexpr uint16_t status(StatusWord word) const {
    switch (word) {
      case StatusWord::STATUS_WORD_CONTROL:
        return this->word(REG_CONTROL_STATUS);
      case StatusWord::STATUS_WORD_BATTERY:
        return this->word(REG_BATTERY_STATUS);
      case StatusWord::STATUS_WORD_OPERATION:
        return this->word(REG_OPERATION_STATUS);
      case StatusWord::STATUS_WORD_GAUGING:
        break;
    }
    return this->gauging_status;
  }
  constexpr bool flag(StatusWord word, uint8_t bit) const { return (this->status(word) >> bit) & 1; }
};

}  // namespace esphome::bq27220_full

// Compile-time tests of bq27220_regs.h. If this file compiles, the tests passed:
//   g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/bq27220_full -fsyntax-only regs_test.cpp
#include "bq27220_regs.h"

using namespace esphome::bq27220_full;

namespace {

constexpr bool is_nan(float x) { return x != x; }
constexpr bool near(float a, float b) { return a - b < 0.0005f && b - a < 0.0005f; }

// ---------------------------------------------------------------- the register blocks

// every documented register is in its block, and the gaps between the blocks are in none
static_assert(block_of(0x00) == 0 && block_of(0x0C) == 0 && block_of(0x0D) == 0 && block_of(0x0E) == -1);
static_assert(block_of(0x10) == 1 && block_of(0x24) == 1 && block_of(0x26) == -1);
static_assert(block_of(0x28) == 2 && block_of(0x36) == 2 && block_of(0x38) == -1);
static_assert(block_of(REG_OPERATION_STATUS) == 3 && block_of(REG_DESIGN_CAPACITY) == 3 && block_of(0x3E) == -1);
// the blocks are in order, do not overlap, and fit the register image
constexpr bool blocks_fit() {
  for (size_t i = 0; i < BLOCKS.size(); i++) {
    if (BLOCKS[i].start + BLOCKS[i].length > REGISTER_SPACE)
      return false;
    if (i > 0 && BLOCKS[i].start < BLOCKS[i - 1].start + BLOCKS[i - 1].length)
      return false;
  }
  return true;
}
static_assert(blocks_fit());
// the validity bits of the blocks and of GaugingStatus fit the byte
static_assert(Reading::GAUGING_STATUS_VALID == 0x10);

// ---------------------------------------------------------------- conversions

static_assert(near(convert(3912, Conversion::CONVERSION_MILLI), 3.912f));          // Voltage 3912 mV
static_assert(near(convert(0xFF9C, Conversion::CONVERSION_MILLI_SIGNED), -0.1f));  // Current -100 mA
static_assert(near(convert(96, Conversion::CONVERSION_MILLI_SIGNED), 0.096f));
static_assert(is_nan(convert(SENTINEL, Conversion::CONVERSION_MILLI_SENTINEL)) &&
              near(convert(4200, Conversion::CONVERSION_MILLI_SENTINEL), 4.2f));
static_assert(near(convert(2982, Conversion::CONVERSION_DECI_KELVIN), 25.05f));  // 298.2 K
static_assert(is_nan(convert(SENTINEL, Conversion::CONVERSION_SENTINEL)) &&
              convert(65534, Conversion::CONVERSION_SENTINEL) == 65534.0f);
static_assert(convert(0x00FD, Conversion::CONVERSION_LOW3) == 5.0f);
static_assert(convert(0xFFFE, Conversion::CONVERSION_SIGNED) == -2.0f &&
              convert(0xFFFE, Conversion::CONVERSION_RAW) == 65534.0f);

// ---------------------------------------------------------------- a reading

constexpr Reading make_reading(uint8_t valid, uint16_t control_status, uint16_t gauging_status) {
  Reading r;
  r.valid = valid;
  r.gauging_status = gauging_status;
  r.registers[REG_CONTROL_STATUS] = control_status & 0xFF;
  r.registers[REG_CONTROL_STATUS + 1] = control_status >> 8;
  r.registers[0x08] = 0x48;  // Voltage 3912 mV
  r.registers[0x09] = 0x0F;
  r.registers[REG_BATTERY_STATUS] = 0x01;      // DSG
  r.registers[REG_BATTERY_STATUS + 1] = 0x80;  // FD
  r.registers[0x16] = 0xFF;                    // TimeToEmpty not applicable
  r.registers[0x17] = 0xFF;
  r.registers[REG_OPERATION_STATUS] = 0x26;  // INITCOMP, SEC = 3
  r.registers[REG_OPERATION_STATUS + 1] = 0x01;
  return r;
}
constexpr uint8_t ALL_BLOCKS = 0x0F;

constexpr Reading ALL = make_reading(ALL_BLOCKS | Reading::GAUGING_STATUS_VALID, 0x0005, 0x0080);
static_assert(near(ALL.value(0x08, Conversion::CONVERSION_MILLI), 3.912f));
static_assert(is_nan(ALL.value(0x16, Conversion::CONVERSION_SENTINEL)));
static_assert(ALL.value(REG_CONTROL_STATUS, Conversion::CONVERSION_LOW3) == 5.0f);  // BATT_ID
static_assert(ALL.flag(StatusWord::STATUS_WORD_BATTERY, 0) && ALL.flag(StatusWord::STATUS_WORD_BATTERY, 15));
static_assert(!ALL.flag(StatusWord::STATUS_WORD_BATTERY, 1));
static_assert(ALL.flag(StatusWord::STATUS_WORD_OPERATION, 5) && ALL.flag(StatusWord::STATUS_WORD_GAUGING, 7));
static_assert(ALL.status_valid(StatusWord::STATUS_WORD_CONTROL) && ALL.status_valid(StatusWord::STATUS_WORD_GAUGING));

// a block that failed gives NaN and invalid status words, the others are still there
constexpr Reading NO_FIRST = make_reading(ALL_BLOCKS & ~1, 0x0005, 0);
static_assert(is_nan(NO_FIRST.value(0x08, Conversion::CONVERSION_MILLI)));
static_assert(!NO_FIRST.status_valid(StatusWord::STATUS_WORD_BATTERY) &&
              !NO_FIRST.status_valid(StatusWord::STATUS_WORD_GAUGING));
static_assert(NO_FIRST.status_valid(StatusWord::STATUS_WORD_OPERATION));
// a register outside the blocks is never valid
static_assert(is_nan(ALL.value(0x0E, Conversion::CONVERSION_RAW)));

// CONTROL_STATUS displaced by a DEVICE_NUMBER or FW_VERSION answer is not a status
constexpr Reading MOVED = make_reading(ALL_BLOCKS, CONTROL_STATUS_MOVED, 0);
static_assert(!MOVED.status_valid(StatusWord::STATUS_WORD_CONTROL) &&
              is_nan(MOVED.value(REG_CONTROL_STATUS, Conversion::CONVERSION_LOW3)));
static_assert(MOVED.status_valid(StatusWord::STATUS_WORD_BATTERY));

// ---------------------------------------------------------------- security access

// OperationStatus 0x0126: SEC = 3, sealed
static_assert(security_access(ALL.status(StatusWord::STATUS_WORD_OPERATION)) == SecurityAccess::SECURITY_ACCESS_SEALED);
static_assert(security_access(0x0002) == SecurityAccess::SECURITY_ACCESS_FULL_ACCESS);
static_assert(security_access(0x0004) == SecurityAccess::SECURITY_ACCESS_UNSEALED);
static_assert(security_access(0x0000) == SecurityAccess::SECURITY_ACCESS_RESERVED);

}  // namespace

int main() { return 0; }

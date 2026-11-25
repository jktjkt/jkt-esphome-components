#pragma once

// SCS message layer: what a valid frame (see scs_decoder.h) means. Standard C++20 only and constexpr, checked by
// tests/components/scs_sniffer/protocol_test.cpp.
//
// There is no public specification; this follows what others reverse-engineered:
//   - lighting/automation commands, state broadcasts, status requests, A5 acknowledgements:
//     Guido Pagani (guidopic.altervista.org/eibscout/logticino.html), flavio/scsgate (messages.py)
//   - action byte table: r0bb10/ESPHome-SCS-Bridge (re/README.md), mined from F461 gateway firmware
//   - door entry ring: stapelberg/intercom-backpack (scsfilter.h), r0bb10
//   - door lock release: r0bb10, matching captures by Michael Stapelberg
// Anything else, including all extended (11-byte) frames, is KIND_UNKNOWN.
//
// Short frames are A8 P0 P1 P2 P3 chk A3. Devices commonly send each frame up to three times.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "scs_decoder.h"

namespace esphome::scs_sniffer {

enum class Kind : uint8_t {
  KIND_UNKNOWN,
  KIND_COMMAND,         // P0 dest, P1 src, P2 = 12, P3 action
  KIND_STATE,           // P0 = B8, P1 src, P2 = 12, P3 action: a device broadcasting its new state
  KIND_STATUS_REQUEST,  // P0 dest, P1 src, P2 = 15
  KIND_SCENARIO,        // P0 src, P2 = 14, P3 scenario number
  KIND_DOOR_RING,       // P0 = 91 (outdoor panel), P1 flat, P2 = 60
  KIND_DOOR_LOCK,       // P0 = 96, P1 = 80 + panel, P2 = 6F, P3 = A4 (open) or A0 (release)
};

static constexpr uint8_t ACK_BYTE = 0xA5;      // single byte outside a frame
static constexpr uint8_t GENERAL_DEST = 0xB1;  // a command addressed to all devices
static constexpr uint8_t STATE_DEST = 0xB8;

/// Buffer size for describe(): the longest text, e.g. "command 33 shutter down (from 00)", and its terminator.
static constexpr std::size_t SUMMARY_SIZE = 40;

struct Message {
  Kind kind = Kind::KIND_UNKNOWN;
  uint8_t dest = 0;   // KIND_COMMAND, KIND_STATUS_REQUEST; flat for KIND_DOOR_RING
  uint8_t src = 0;    // KIND_COMMAND, KIND_STATE, KIND_STATUS_REQUEST, KIND_SCENARIO; panel for KIND_DOOR_LOCK
  uint8_t value = 0;  // action for KIND_COMMAND/KIND_STATE, scenario for KIND_SCENARIO, P3 otherwise
};

constexpr Message parse(const Frame &f) {
  if (f.size != SHORT_FRAME_SIZE)
    return {};
  const uint8_t p0 = f[1], p1 = f[2], p2 = f[3], p3 = f[4];
  if (p0 == 0x91 && p2 == 0x60)
    return {Kind::KIND_DOOR_RING, p1, p0, p3};
  if (p0 == 0x96 && p1 >= 0x80 && p2 == 0x6F && (p3 == 0xA4 || p3 == 0xA0))
    return {Kind::KIND_DOOR_LOCK, 0, static_cast<uint8_t>(p1 - 0x80), p3};
  switch (p2) {
    case 0x12:
      return p0 == STATE_DEST ? Message{Kind::KIND_STATE, 0, p1, p3} : Message{Kind::KIND_COMMAND, p0, p1, p3};
    case 0x14:
      return {Kind::KIND_SCENARIO, 0, p0, p3};
    case 0x15:
      return {Kind::KIND_STATUS_REQUEST, p0, p1, p3};
  }
  return {};
}

namespace detail {

/// Appends `text` at `pos`, cut to fit with a terminating NUL, and returns the new position.
constexpr std::size_t append(std::span<char> out, std::size_t pos, std::string_view text) {
  for (char c : text) {
    if (pos + 1 >= out.size())
      break;
    out[pos++] = c;
  }
  out[pos] = '\0';
  return pos;
}

constexpr std::size_t append_hex(std::span<char> out, std::size_t pos, uint8_t b) {
  constexpr std::string_view digits = "0123456789ABCDEF";
  const char text[] = {digits[b >> 4], digits[b & 0x0F]};
  return append(out, pos, {text, sizeof text});
}

constexpr std::size_t append_decimal(std::span<char> out, std::size_t pos, unsigned value) {
  char text[10] = {};
  std::size_t start = sizeof text;
  do {
    text[--start] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  return append(out, pos, {text + start, sizeof text - start});
}

/// Lighting and shutter action byte (KIND_COMMAND and KIND_STATE).
constexpr std::size_t append_action(std::span<char> out, std::size_t pos, uint8_t a) {
  switch (a) {
    case 0x00:
      return append(out, pos, "on");
    case 0x01:
      return append(out, pos, "off");
    case 0x03:
      return append(out, pos, "dim up");
    case 0x04:
      return append(out, pos, "dim down");
    case 0x08:
      return append(out, pos, "shutter up");
    case 0x09:
      return append(out, pos, "shutter down");
    case 0x0A:
      return append(out, pos, "shutter stop");
  }
  switch (a & 0x0F) {
    case 0x0D:  // preset level, 1D..9D = 20..100 %
      pos = append(out, pos, "level ");
      pos = append_decimal(out, pos, ((a >> 4) + 1) * 10);
      return append(out, pos, "%");
    case 0x06:  // timed on; the mapping to a duration is not known
      pos = append(out, pos, "timed on ");
      return append_hex(out, pos, a);
    case 0x0B:
      pos = append(out, pos, "blink ");
      return append_hex(out, pos, a);
  }
  pos = append(out, pos, "action ");
  return append_hex(out, pos, a);
}

}  // namespace detail

/// Writes a short text such as "command 33 on (from 00)", "ring flat 03" or "door lock 20 open" into `out`
/// (SUMMARY_SIZE bytes hold any of them), terminated by a NUL, and returns its length.
constexpr std::size_t describe(const Message &m, std::span<char> out) {
  using detail::append;
  using detail::append_hex;
  std::size_t pos = 0;
  switch (m.kind) {
    case Kind::KIND_COMMAND:
      pos = append(out, pos, "command ");
      pos = m.dest == GENERAL_DEST ? append(out, pos, "all") : append_hex(out, pos, m.dest);
      pos = append(out, pos, " ");
      pos = detail::append_action(out, pos, m.value);
      pos = append(out, pos, " (from ");
      pos = append_hex(out, pos, m.src);
      return append(out, pos, ")");
    case Kind::KIND_STATE:
      pos = append(out, pos, "state ");
      pos = append_hex(out, pos, m.src);
      pos = append(out, pos, " ");
      return detail::append_action(out, pos, m.value);
    case Kind::KIND_STATUS_REQUEST:
      pos = append(out, pos, "status request ");
      pos = append_hex(out, pos, m.dest);
      pos = append(out, pos, " (from ");
      pos = append_hex(out, pos, m.src);
      return append(out, pos, ")");
    case Kind::KIND_SCENARIO:
      pos = append(out, pos, "scenario ");
      pos = append_hex(out, pos, m.value);
      pos = append(out, pos, " (from ");
      pos = append_hex(out, pos, m.src);
      return append(out, pos, ")");
    case Kind::KIND_DOOR_RING:
      pos = append(out, pos, "ring flat ");
      return append_hex(out, pos, m.dest);
    case Kind::KIND_DOOR_LOCK:
      pos = append(out, pos, "door lock ");
      pos = append_hex(out, pos, m.src);
      return append(out, pos, m.value == 0xA4 ? " open" : " release");
    case Kind::KIND_UNKNOWN:
      break;
  }
  return append(out, pos, "unknown");
}

}  // namespace esphome::scs_sniffer

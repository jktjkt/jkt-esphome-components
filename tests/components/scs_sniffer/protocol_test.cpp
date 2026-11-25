// Compile-time tests for scs_protocol.h. If this file compiles, the tests passed:
//   g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/scs_sniffer -fsyntax-only protocol_test.cpp
#include "scs_protocol.h"

#include <array>
#include <string_view>

namespace scs = esphome::scs_sniffer;

namespace {

using scs::Kind;

constexpr scs::Frame frame(uint8_t p0, uint8_t p1, uint8_t p2, uint8_t p3) {
  scs::Frame f;
  f.data = {scs::FRAME_START, p0, p1, p2, p3, static_cast<uint8_t>(p0 ^ p1 ^ p2 ^ p3), scs::FRAME_END};
  f.size = scs::SHORT_FRAME_SIZE;
  return f;
}

struct Summary {
  std::array<char, scs::SUMMARY_SIZE> text{};
  std::size_t length = 0;
  constexpr std::string_view view() const { return {this->text.data(), this->length}; }
};

constexpr Summary summary(const scs::Message &m) {
  Summary s;
  s.length = scs::describe(m, s.text);
  return s;
}

// The text, its length and its terminating NUL agree
constexpr bool says(const scs::Frame &f, std::string_view text) {
  const Summary s = summary(scs::parse(f));
  return s.view() == text && s.text[s.length] == '\0';
}

// Published examples (Pagani, scsgate): A8 33 00 12 00 21 A3 is "33 on".
static_assert(frame(0x33, 0x00, 0x12, 0x00).data[5] == 0x21);
static_assert(frame(0x33, 0x00, 0x12, 0x01).data[5] == 0x20);

constexpr auto ON = scs::parse(frame(0x33, 0x00, 0x12, 0x00));
static_assert(ON.kind == Kind::KIND_COMMAND && ON.dest == 0x33 && ON.src == 0x00 && ON.value == 0x00);
static_assert(says(frame(0x33, 0x00, 0x12, 0x00), "command 33 on (from 00)"));
static_assert(says(frame(0x33, 0x00, 0x12, 0x01), "command 33 off (from 00)"));
static_assert(says(frame(0xB1, 0x00, 0x12, 0x01), "command all off (from 00)"));
static_assert(summary(ON).length == 23);
static_assert(says(frame(0x33, 0x00, 0x12, 0x4D), "command 33 level 50% (from 00)"));
static_assert(says(frame(0x33, 0x00, 0x12, 0x9D), "command 33 level 100% (from 00)"));
static_assert(says(frame(0x33, 0x00, 0x12, 0x03), "command 33 dim up (from 00)"));
static_assert(says(frame(0x41, 0x00, 0x12, 0x0A), "command 41 shutter stop (from 00)"));
static_assert(says(frame(0x33, 0x00, 0x12, 0x26), "command 33 timed on 26 (from 00)"));
static_assert(says(frame(0x33, 0x00, 0x12, 0x7F), "command 33 action 7F (from 00)"));

// State broadcast after a command: A8 B8 33 12 01 98 A3.
static_assert(frame(0xB8, 0x33, 0x12, 0x01).data[5] == 0x98);
static_assert(scs::parse(frame(0xB8, 0x33, 0x12, 0x01)).kind == Kind::KIND_STATE);
static_assert(says(frame(0xB8, 0x33, 0x12, 0x01), "state 33 off"));

// Status request: A8 33 00 15 00 26 A3.
static_assert(frame(0x33, 0x00, 0x15, 0x00).data[5] == 0x26);
static_assert(says(frame(0x33, 0x00, 0x15, 0x00), "status request 33 (from 00)"));

static_assert(says(frame(0x21, 0x00, 0x14, 0x05), "scenario 05 (from 21)"));

// Door entry: ring from the outdoor panel to flat 03, door lock open / release on panel 20.
constexpr auto RING = scs::parse(frame(0x91, 0x03, 0x60, 0x08));
static_assert(RING.kind == Kind::KIND_DOOR_RING && RING.dest == 0x03);
static_assert(says(frame(0x91, 0x03, 0x60, 0x08), "ring flat 03"));
static_assert(says(frame(0x96, 0xA0, 0x6F, 0xA4), "door lock 20 open"));
static_assert(says(frame(0x96, 0xA1, 0x6F, 0xA0), "door lock 21 release"));

// Seen on buses, meaning unknown; and extended frames.
static_assert(says(frame(0x98, 0xA0, 0x6F, 0xA3), "unknown"));
static_assert(says(frame(0x96, 0xA0, 0x6F, 0x01), "unknown"));
static_assert(says(frame(0x96, 0x20, 0x6F, 0xA4), "unknown"));  // panel byte below 0x80
constexpr scs::Frame EXTENDED = [] {
  scs::Frame f;
  f.data = {0xA8, 0xD1, 0x53, 0x01, 0x42, 0x0D, 0x00, 0x00, 0x02, 0xCE, 0xA3};
  f.size = scs::LONG_FRAME_SIZE;
  return f;
}();
static_assert(scs::parse(EXTENDED).kind == Kind::KIND_UNKNOWN);

// SUMMARY_SIZE holds every text: the longest actions with both kinds of destination, and every other kind.
constexpr bool all_fit() {
  for (unsigned action = 0; action < 256; ++action)
    for (uint8_t dest : {uint8_t{0x33}, scs::GENERAL_DEST})
      for (Kind kind : {Kind::KIND_COMMAND, Kind::KIND_STATE})
        if (summary({kind, dest, 0x00, static_cast<uint8_t>(action)}).length + 1 >= scs::SUMMARY_SIZE)
          return false;
  for (Kind kind :
       {Kind::KIND_UNKNOWN, Kind::KIND_STATUS_REQUEST, Kind::KIND_SCENARIO, Kind::KIND_DOOR_RING, Kind::KIND_DOOR_LOCK})
    if (summary({kind, 0xFF, 0xFF, 0xA0}).length + 1 >= scs::SUMMARY_SIZE)
      return false;
  return true;
}
static_assert(all_fit());
static_assert(says(frame(0xB1, 0x00, 0x12, 0x09), "command all shutter down (from 00)"));

// A buffer that is too short gets the start of the text, still terminated.
constexpr bool cut_short() {
  std::array<char, 8> text{};
  const std::size_t length = scs::describe(scs::parse(frame(0x33, 0x00, 0x12, 0x00)), text);
  return length == 7 && std::string_view(text.data(), length) == "command" && text[7] == '\0';
}
static_assert(cut_short());

}  // namespace

int main() { return 0; }

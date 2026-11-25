// Compile-time tests of poseidon_packet.h. If this file compiles, the tests passed:
//   g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/enika_poseidon -fsyntax-only decode_test.cpp
//
// The state message frames are made up: the transmitter ID is one shown in the Poseidon user guide (5F1B9A, a
// P8 T 4 UNI), and the 4 trailer bytes are filler, not a valid hash. The counter is masked with them, so it is
// whatever they unmask to. The decoder does not check the hash.
#include "poseidon_packet.h"

using namespace esphome::enika_poseidon;

namespace {

template<size_t N> using Bytes = std::array<uint8_t, N>;

constexpr StateMessage decoded(std::span<const uint8_t> frame) {
  StateMessage m;
  decode_state_message(frame, m);
  return m;
}
constexpr bool decodes(std::span<const uint8_t> frame) {
  StateMessage m;
  return decode_state_message(frame, m);
}
// The summary is NUL terminated and nothing else
constexpr bool summary_is(std::span<const uint8_t> frame, std::string_view expected) {
  std::array<char, SUMMARY_SIZE> buf{};
  buf.fill('#');
  const size_t len = summarize(frame, buf);
  return std::string_view(buf.data(), len) == expected && buf[len] == '\0';
}
constexpr bool mask_is(uint8_t flags, std::string_view expected) {
  const auto mask = flags_mask(flags);
  return std::string_view(mask.data(), mask.size()) == expected;
}

// a fob press: left button, short, first frame, counter 13260
constexpr Bytes<11> PRESS{0x0A, 0x40, 0x2C, 0x5F, 0x1B, 0x9A, 0x04, 0x01, 0x23, 0x45, 0x67};
constexpr StateMessage M = decoded(PRESS);
static_assert(decodes(PRESS) && classify(PRESS) == PacketKind::PACKET_KIND_STATE);
static_assert(M.id == 0x5F1B9A && M.cnt3 == 4 && M.msg_type == 0 && !M.init);
static_assert(M.trailer_format == TRAILER_COUNTER_HASH);
static_assert(M.has_counter && M.counter == 13260 && M.hash == 0x4567);
static_assert(M.left() && !M.right() && !M.repeat() && !M.long_press());
static_assert(M.route().empty() && M.payload().empty());
static_assert(summary_is(PRESS, "5F1B9A L.... 13260"));

// its repeat frame: same counter, other filler
constexpr Bytes<11> REPEAT{0x0A, 0x40, 0x2C, 0x5F, 0x1B, 0x9A, 0x14, 0x89, 0xAB, 0xCD, 0xEF};
static_assert(decoded(REPEAT).counter == 13260 && decoded(REPEAT).repeat());
static_assert(summary_is(REPEAT, "5F1B9A L..r. 13260"));

// an init frame: ctrl bit 7, the counter reads 0xFFFF
constexpr Bytes<11> INIT{0x0A, 0x30, 0xAC, 0x5F, 0x1B, 0x9A, 0x14, 0x89, 0x45, 0x10, 0x23};
static_assert(decoded(INIT).init && decoded(INIT).counter == 0xFFFF && decoded(INIT).cnt3 == 3);
static_assert(summary_is(INIT, "5F1B9A L..r. init"));

// through a repeater: two route bytes, a one byte trailer, and a payload
constexpr Bytes<11> ROUTED{0x0A, 0x01, 0x12, 0xAA, 0xBB, 0x5F, 0x1B, 0x9A, 0x08, 0x77, 0x99};
constexpr StateMessage R = decoded(ROUTED);
static_assert(R.route().size() == 2 && R.route()[0] == 0xAA && R.route()[1] == 0xBB);
static_assert(R.id == 0x5F1B9A && R.right() && R.trailer_format == TRAILER_ONE_BYTE && R.trailer_byte == 0x99);
static_assert(R.payload().size() == 1 && R.payload()[0] == 0x77 && !R.has_counter);
static_assert(summary_is(ROUTED, "5F1B9A .R... via"));

// a copy keeps its bytes, as an automation that defers keeps the message
constexpr bool copy_keeps_bytes() {
  StateMessage copy;
  {
    const StateMessage original = decoded(ROUTED);
    copy = original;
  }
  return copy.route()[1] == 0xBB && copy.payload()[0] == 0x77;
}
static_assert(copy_keeps_bytes());

// not a state message, or too short for one
constexpr Bytes<12> SEARCH{0x0B, 0x84, 0x00, 0x00, 0x80, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0xFF};
static_assert(!decodes(SEARCH) && classify(SEARCH) == PacketKind::PACKET_KIND_SEARCH_REQUEST);
static_assert(summary_is(SEARCH, "search 050301-060402"));
constexpr Bytes<5> SHORT_STATE{0x04, 0x00, 0x2C, 0x5F, 0x1B};
static_assert(!decodes(SHORT_STATE));
constexpr Bytes<3> BAD_LENGTH{0x0A, 0x50, 0x2C};
static_assert(classify(BAD_LENGTH) == PacketKind::PACKET_KIND_UNKNOWN && !decodes(BAD_LENGTH));
static_assert(summary_is(BAD_LENGTH, "?? type 50 len 2"));
static_assert(summary_is(std::span<const uint8_t>(BAD_LENGTH).first(1), "?? len 1"));
constexpr Bytes<7> REPLY{0x06, 0x85, 0x00, 0x12, 0x34, 0x56, 0x00};
static_assert(summary_is(REPLY, "reply 123456"));
constexpr Bytes<4> SCAN{0x03, 0xC0, 0x00, 0xC8};
static_assert(summary_is(SCAN, "range scan 200"));
constexpr Bytes<9> REMOTE{0x08, 0x91, 0x00, 0xAB, 0xCD, 0xEF, 0, 0, 0};
static_assert(summary_is(REMOTE, "remote 91 rep ABCDEF"));

// the mask: right button held, repeat frame; both buttons; a bit nobody knows
static_assert(mask_is(0x08, ".R..."));
static_assert(mask_is(0x38, ".RHr."));
static_assert(mask_is(0x0C, "LR..."));
static_assert(mask_is(0x00, "....."));
static_assert(mask_is(0x45, "L...?"));
constexpr Bytes<11> ODD_FLAGS{0x0A, 0x40, 0x2C, 0x5F, 0x1B, 0x9A, 0x45, 0x01, 0x23, 0x45, 0x67};
static_assert(summary_is(ODD_FLAGS, "5F1B9A L...? 13260 f45"));

// the longest summary fits: every flag, the largest counter, a route
constexpr Bytes<12> LONGEST{0x0B, 0x40, 0x2D, 0x01, 0x5F, 0x1B, 0x9A, 0xFF, 0x00, 0x00, 0x55, 0xAA};
static_assert(summary_is(LONGEST, "5F1B9A LRHr? 65535 fFF via"));

// the longest frame decodes, with all of it in the payload
constexpr bool longest_frame() {
  std::array<uint8_t, MAX_FRAME> frame{};
  frame[0] = MAX_FRAME - 1;
  frame[1] = 0x40;
  frame[2] = 0x00;  // no route, no trailer
  frame[MAX_FRAME - 1] = 0x5A;
  StateMessage m;
  if (!frame_valid(frame) || !decode_state_message(frame, m))
    return false;
  return m.payload().size() == MAX_PAYLOAD && m.payload().back() == 0x5A;
}
static_assert(longest_frame());

}  // namespace

int main() { return 0; }

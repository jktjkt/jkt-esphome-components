#pragma once

// Enika Poseidon 868 MHz radio packets, reverse engineered via an SDR.
// A frame is <length> <length data bytes>, without preamble, sync word and CRC.
// Standard library only and constexpr, so that tests/components/enika_poseidon/decode_test.cpp can check it with
// static_asserts on the host. Nothing here allocates.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace esphome::enika_poseidon {

/// The length byte is 8 bits: a frame is the length byte and at most 255 more
static constexpr size_t MAX_FRAME = 256;
/// ctrl bits 1:0 count the route bytes
static constexpr size_t MAX_ROUTE = 3;
/// What is left of the longest frame after the length, type, ctrl, ID and flags bytes
static constexpr size_t MAX_PAYLOAD = MAX_FRAME - 7;
/// The longest summary line and its NUL, see summarize()
static constexpr size_t SUMMARY_SIZE = 32;

enum class PacketKind : uint8_t {
  PACKET_KIND_UNKNOWN,
  PACKET_KIND_STATE,
  PACKET_KIND_SEARCH_REQUEST,
  PACKET_KIND_SEARCH_REPLY,
  PACKET_KIND_RANGE_SCAN,
  PACKET_KIND_REMOTE_MANAGEMENT,
};

// The values of StateMessage::trailer_format, ctrl bits 5:4
static constexpr uint8_t TRAILER_NONE = 0;
static constexpr uint8_t TRAILER_ONE_BYTE = 1;
static constexpr uint8_t TRAILER_COUNTER_HASH = 2;
static constexpr uint8_t TRAILER_ENCRYPTED = 3;

static constexpr uint8_t FLAG_LEFT = 0x04;
static constexpr uint8_t FLAG_RIGHT = 0x08;
static constexpr uint8_t FLAG_REPEAT = 0x10;
static constexpr uint8_t FLAG_LONG = 0x20;
static constexpr uint8_t KNOWN_FLAGS = FLAG_LEFT | FLAG_RIGHT | FLAG_REPEAT | FLAG_LONG;

/// A transmitter's state message (byte 1 below 0x80), e.g. a P8 T2 key fob press.
/// Every bit of the frame is in some field: byte1, ctrl and flags are kept raw next to the fields cut out of them.
/// It holds copies of its bytes, so it stays valid when an automation keeps it for later.
struct StateMessage {
  uint32_t id{0};
  uint16_t counter{0};  // unmasked, 0xFFFF in init frames
  uint16_t hash{0};
  uint8_t byte1{0};
  uint8_t ctrl{0};
  uint8_t flags{0};
  uint8_t msg_type{0};  // byte 1 bits 3:0
  uint8_t cnt3{0};      // byte 1 bits 6:4, the low 3 bits of the counter; init frames: a separate init counter
  uint8_t trailer_format{TRAILER_NONE};
  uint8_t trailer_byte{0};  // trailer format 1
  bool init{false};         // ctrl bit 7
  bool has_counter{false};  // trailer format 2
  uint8_t route_size{0};
  uint8_t payload_size{0};
  std::array<uint8_t, MAX_ROUTE> route_bytes{};
  std::array<uint8_t, MAX_PAYLOAD> payload_bytes{};

  /// Bytes a repeater inserted after ctrl
  constexpr std::span<const uint8_t> route() const { return {this->route_bytes.data(), this->route_size}; }
  /// Bytes between the flags and the trailer
  constexpr std::span<const uint8_t> payload() const { return {this->payload_bytes.data(), this->payload_size}; }
  constexpr bool left() const { return this->flags & FLAG_LEFT; }
  constexpr bool right() const { return this->flags & FLAG_RIGHT; }
  constexpr bool repeat() const { return this->flags & FLAG_REPEAT; }
  constexpr bool long_press() const { return this->flags & FLAG_LONG; }
};

/// True if frame[0] counts the bytes after it and there is a type byte.
constexpr bool frame_valid(std::span<const uint8_t> frame) { return frame.size() >= 2 && frame[0] == frame.size() - 1; }

constexpr PacketKind classify(std::span<const uint8_t> frame) {
  if (!frame_valid(frame))
    return PacketKind::PACKET_KIND_UNKNOWN;
  const uint8_t type = frame[1];
  if (type < 0x80)
    return PacketKind::PACKET_KIND_STATE;
  if (type == 0x84)
    return PacketKind::PACKET_KIND_SEARCH_REQUEST;
  if (type == 0x85)
    return PacketKind::PACKET_KIND_SEARCH_REPLY;
  if (type == 0xC0)
    return PacketKind::PACKET_KIND_RANGE_SCAN;
  if (type >= 0x90 && type <= 0xBF)
    return PacketKind::PACKET_KIND_REMOTE_MANAGEMENT;
  return PacketKind::PACKET_KIND_UNKNOWN;
}

/// Decodes a state message, false if the frame is not one or too short. `out` is only written on success.
constexpr bool decode_state_message(std::span<const uint8_t> frame, StateMessage &out) {
  const size_t len = frame.size();
  if (classify(frame) != PacketKind::PACKET_KIND_STATE || len < 3)
    return false;
  const uint8_t ctrl = frame[2];
  const size_t route_count = ctrl & 3;
  const size_t id_at = 3 + route_count;
  if (len < id_at + 4)  // ID (3 bytes) and flags
    return false;
  StateMessage m;
  m.byte1 = frame[1];
  m.ctrl = ctrl;
  m.msg_type = frame[1] & 0x0F;
  m.cnt3 = (frame[1] >> 4) & 7;
  m.init = ctrl >> 7;
  m.trailer_format = (ctrl >> 4) & 3;
  m.route_size = route_count;
  for (size_t i = 0; i < route_count; i++)
    m.route_bytes[i] = frame[3 + i];
  m.id = uint32_t(frame[id_at]) << 16 | uint32_t(frame[id_at + 1]) << 8 | frame[id_at + 2];
  m.flags = frame[id_at + 3];
  auto body = frame.subspan(id_at + 4);
  if (m.trailer_format == TRAILER_COUNTER_HASH && body.size() >= 4) {
    // The counter is XOR-masked with the hash bytes
    const auto t = body.last(4);
    m.has_counter = true;
    m.counter = uint16_t(t[0] ^ t[3] ^ 0x55) << 8 | uint16_t(t[1] ^ t[2] ^ 0xAA);
    m.hash = uint16_t(t[2]) << 8 | t[3];
    body = body.first(body.size() - 4);
  } else if (m.trailer_format == TRAILER_ONE_BYTE && !body.empty()) {
    m.trailer_byte = body.back();
    body = body.first(body.size() - 1);
  }
  m.payload_size = body.size();
  for (size_t i = 0; i < body.size(); i++)
    m.payload_bytes[i] = body[i];
  out = m;
  return true;
}

/// The flags byte as a mask of 5 characters, a dot for each bit that is not set:
/// L left button, R right button, H long press (hold), r repeat frame, ? any other bit of the byte.
constexpr std::array<char, 5> flags_mask(uint8_t flags) {
  return {(flags & FLAG_LEFT) ? 'L' : '.', (flags & FLAG_RIGHT) ? 'R' : '.', (flags & FLAG_LONG) ? 'H' : '.',
          (flags & FLAG_REPEAT) ? 'r' : '.', (flags & ~KNOWN_FLAGS) ? '?' : '.'};
}

namespace detail {

/// Appends to a summary, cutting off what does not fit before its NUL.
class SummaryWriter {
 public:
  constexpr explicit SummaryWriter(std::span<char, SUMMARY_SIZE> out) : out_(out) {}

  constexpr SummaryWriter &text(std::string_view s) {
    for (const char c : s) {
      if (this->len_ < SUMMARY_SIZE - 1)
        this->out_[this->len_++] = c;
    }
    return *this;
  }
  constexpr SummaryWriter &hex(uint32_t value, int digits) {
    while (digits-- > 0)
      this->text({&"0123456789ABCDEF"[(value >> (4 * digits)) & 0x0F], 1});
    return *this;
  }
  constexpr SummaryWriter &dec(uint32_t value) {
    char digits[10]{};
    int n = 0;
    do {
      digits[n++] = char('0' + value % 10);
      value /= 10;
    } while (value != 0);
    while (n > 0)
      this->text({&digits[--n], 1});
    return *this;
  }
  /// Terminates the text and returns its length
  constexpr size_t finish() {
    this->out_[this->len_] = '\0';
    return this->len_;
  }

 protected:
  std::span<char, SUMMARY_SIZE> out_;
  size_t len_{0};
};

}  // namespace detail

/// Writes one NUL terminated line into `out` and returns its length.
/// A state message is <ID> <flags mask> <counter>, fit for a 27 column screen, see flags_mask(). An init frame shows
/// "init" instead of the counter, flags outside the four known bits are added as " fXX", a route as " via".
/// Other packet types show their type and the ID where there is one.
constexpr size_t summarize(std::span<const uint8_t> frame, std::span<char, SUMMARY_SIZE> out) {
  const size_t len = frame.size();
  detail::SummaryWriter s(out);
  StateMessage m;
  if (decode_state_message(frame, m)) {
    const auto mask = flags_mask(m.flags);
    s.hex(m.id, 6).text(" ").text({mask.data(), mask.size()});
    if (m.init) {
      s.text(" init");
    } else if (m.has_counter) {
      s.text(" ").dec(m.counter);
    }
    if (m.flags & ~KNOWN_FLAGS)
      s.text(" f").hex(m.flags, 2);
    if (m.route_size != 0)
      s.text(" via");
    return s.finish();
  }
  switch (classify(frame)) {
    case PacketKind::PACKET_KIND_SEARCH_REQUEST:
      if (len >= 12) {
        const uint32_t ida = uint32_t(frame[9]) << 16 | frame[7] << 8 | frame[5];
        const uint32_t idb = uint32_t(frame[10]) << 16 | frame[8] << 8 | frame[6];
        return s.text("search ").hex(ida, 6).text("-").hex(idb, 6).finish();
      }
      break;
    case PacketKind::PACKET_KIND_SEARCH_REPLY:
      if (len >= 7)
        return s.text("reply ").hex(frame[3], 2).hex(frame[4], 2).hex(frame[5], 2).finish();
      break;
    case PacketKind::PACKET_KIND_RANGE_SCAN:
      if (len >= 4)
        return s.text("range scan ").dec(frame[3]).finish();
      break;
    case PacketKind::PACKET_KIND_REMOTE_MANAGEMENT:
      if (len >= 9) {
        return s.text("remote ")
            .hex(frame[1], 2)
            .text((frame[1] & 1) ? " rep " : " req ")
            .hex(frame[3], 2)
            .hex(frame[4], 2)
            .hex(frame[5], 2)
            .finish();
      }
      break;
    default:
      break;
  }
  if (len >= 2)
    return s.text("?? type ").hex(frame[1], 2).text(" len ").dec(uint32_t(len - 1)).finish();
  return s.text("?? len ").dec(uint32_t(len)).finish();
}

}  // namespace esphome::enika_poseidon

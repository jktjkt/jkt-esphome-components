// Compile-time tests of hci.h. If this file compiles, the tests passed:
//   g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/im891a_xl -fsyntax-only hci_test.cpp
//
// The ping frames are the examples of the HCI specification, version 2.4. The configuration frame is what a working
// receiver sends. The received packet is made up: manufacturer ABC, device ID 12345678.
#include "hci.h"

#include <algorithm>
#include <initializer_list>
#include <string_view>
#include <vector>

using namespace esphome::im891a_xl::hci;

namespace {

// A failing check calls this, which is not constexpr, so the compiler names the check in its error
void check_failed(const char *) {}
#define CHECK(cond) \
  do { \
    if (!(cond)) \
      check_failed(#cond); \
  } while (0)

// The tests allocate, which constexpr allows as long as it is freed before the end of the evaluation. hci.h does not.
using Bytes = std::vector<uint8_t>;

constexpr bool same(std::span<const uint8_t> a, std::span<const uint8_t> b) { return std::ranges::equal(a, b); }
constexpr bool same(std::span<const uint8_t> a, std::initializer_list<uint8_t> b) { return std::ranges::equal(a, b); }

// The text in a NUL terminated array
template<size_t N> constexpr std::string_view text(const std::array<char, N> &s) { return s.data(); }

// The frame of a message, in a buffer of frame_capacity() of its payload
constexpr Bytes enc(uint8_t sap, uint8_t id, std::span<const uint8_t> payload = {}) {
  Bytes frame(frame_capacity(payload.size()));
  frame.resize(encode(frame, sap, id, payload));
  return frame;
}
template<size_t P> constexpr Bytes enc(uint8_t sap, uint8_t id, const std::array<uint8_t, P> &payload) {
  return enc(sap, id, std::span<const uint8_t>(payload));
}

// A message copied out of the decoder, whose payload is only valid until the next byte
struct Got {
  uint8_t sap;
  uint8_t id;
  Bytes payload;
};

// Feeds the bytes, returns the messages that came out
constexpr std::vector<Got> feed(SlipDecoder &d, std::span<const uint8_t> bytes) {
  std::vector<Got> out;
  for (const uint8_t b : bytes) {
    if (d.push(b)) {
      const Message &m = d.message();
      out.push_back({m.sap, m.id, Bytes(m.payload.begin(), m.payload.end())});
    }
  }
  return out;
}
constexpr std::vector<Got> feed(SlipDecoder &d, std::initializer_list<uint8_t> bytes) {
  return feed(d, std::span<const uint8_t>(bytes.begin(), bytes.size()));
}

// ---------------------------------------------------------------- CRC

// The check value of CRC-16/IBM-SDLC is that of "123456789", and the body of the ping request of the specification
// (SAP 01, message 01) has the CRC 0x0716, which goes out LSB first as 16 07.
constexpr std::array<uint8_t, 9> CRC_CHECK_INPUT{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
static_assert(crc16(CRC_CHECK_INPUT) == 0x906E);
constexpr std::array<uint8_t, 2> PING_BODY{0x01, 0x01};
static_assert(crc16(PING_BODY) == 0x0716);
static_assert(crc16({}) == 0x0000);
// in pieces
static_assert(crc16(std::span(CRC_CHECK_INPUT).subspan(4), crc16(std::span(CRC_CHECK_INPUT).first(4), 0xFFFF, false)) ==
              0x906E);

// ---------------------------------------------------------------- encoding

// the room a frame needs at most
static_assert(frame_capacity(0) == 10);
static_assert(frame_capacity(GatewayConfig::SIZE) == 32);
static_assert(frame_capacity(MAX_PAYLOAD_SIZE) == 1010);

// the ping request and response of the specification
static_assert(same(enc(SAP_DEVICE_MANAGEMENT, DEVMGMT_PING_REQ), {0xC0, 0x01, 0x01, 0x16, 0x07, 0xC0}));
static_assert(same(enc(SAP_DEVICE_MANAGEMENT, DEVMGMT_PING_RSP, std::array<uint8_t, 1>{STATUS_OK}),
                   {0xC0, 0x01, 0x02, 0x00, 0xA0, 0xAF, 0xC0}));
// StartBootloader: not in the specification, the frames are those the module and a RAM image work with
static_assert(same(enc(SAP_DEVICE_MANAGEMENT, DEVMGMT_START_BOOTLOADER_REQ), {0xC0, 0x01, 0xF1, 0x99, 0xF0, 0xC0}));
static_assert(same(enc(SAP_DEVICE_MANAGEMENT, DEVMGMT_START_BOOTLOADER_RSP, std::array<uint8_t, 1>{STATUS_OK}),
                   {0xC0, 0x01, 0xF2, 0x00, 0xA8, 0xD3, 0xC0}));
// an escaped ESC and END, and a header that is escaped too, as the encoder of the earlier version had them
static_assert(same(enc(0x09, 0x55, std::array<uint8_t, 3>{0xDB, 0xC0, 0xDB}),
                   {0xC0, 0x09, 0x55, 0xDB, 0xDD, 0xDB, 0xDC, 0xDB, 0xDD, 0x0F, 0x21, 0xC0}));
static_assert(same(enc(0xC0, 0xDB, std::array<uint8_t, 4>{0xC0, 0xDB, 0xC0, 0xDB}),
                   {0xC0, 0xDB, 0xDC, 0xDB, 0xDD, 0xDB, 0xDC, 0xDB, 0xDD, 0xDB, 0xDC, 0xDB, 0xDD, 0xA3, 0x77, 0xC0}));
// a frame fits a buffer of exactly its length, and one byte less gives nothing
constexpr bool exact_fit() {
  const std::array<uint8_t, 3> payload{0xDB, 0xC0, 0xDB};
  std::array<uint8_t, 12> buf{};
  CHECK(encode(buf, 0x09, 0x55, payload) == 12);
  CHECK(encode(std::span(buf).first(11), 0x09, 0x55, payload) == 0);
  CHECK(encode(std::span<uint8_t>(), 0x01, 0x01) == 0);
  return true;
}
static_assert(exact_fit());
// a header and a payload that are all escaped fit, and come back
constexpr bool all_escaped() {
  const std::array<uint8_t, 4> payload{0xC0, 0xDB, 0xC0, 0xDB};
  const auto frame = enc(0xC0, 0xDB, payload);
  CHECK(frame.size() >= 2 + 2 * 6 && frame.size() <= frame_capacity(payload.size()));
  SlipDecoder d;
  const auto m = feed(d, frame);
  CHECK(m.size() == 1 && m[0].sap == 0xC0 && m[0].id == 0xDB && same(m[0].payload, payload));
  return true;
}
static_assert(all_escaped());

// ---------------------------------------------------------------- decoding

constexpr bool ping() {
  SlipDecoder d;
  const auto m = feed(d, {0xC0, 0x01, 0x02, 0x00, 0xA0, 0xAF, 0xC0});
  CHECK(m.size() == 1 && m[0].sap == 1 && m[0].id == 2 && same(m[0].payload, {0x00}));
  const auto b = feed(d, {0xC0, 0x01, 0xF2, 0x00, 0xA8, 0xD3, 0xC0});
  CHECK(b.size() == 1 && b[0].id == DEVMGMT_START_BOOTLOADER_RSP && same(b[0].payload, {0x00}));
  return true;
}
static_assert(ping());

// The payload is a view into the decoder, valid until the next byte
constexpr bool payload_is_a_view() {
  SlipDecoder d;
  for (const uint8_t b : {0xC0, 0x01, 0x02, 0x00, 0xA0, 0xAF})
    CHECK(!d.push(b));
  CHECK(d.push(0xC0));
  CHECK(same(d.message().payload, {0x00}));
  d.reset();
  CHECK(d.message().payload.empty());
  return true;
}
static_assert(payload_is_a_view());

// split over several reads, without the leading END, with several messages in a row
constexpr bool split() {
  SlipDecoder d;
  CHECK(feed(d, {0xC0, 0x01}).empty());
  CHECK(feed(d, {0x01, 0x16, 0x07, 0xC0}).size() == 1);
  CHECK(feed(d, {0x01, 0x01, 0x16, 0x07, 0xC0}).size() == 1);
  const auto m = feed(d, {0xC0, 0x01, 0x02, 0x00, 0xA0, 0xAF, 0xC0, 0xC0, 0x01, 0x02, 0x01, 0x29, 0xBE, 0xC0});
  CHECK(m.size() == 2 && same(m[0].payload, {0x00}) && same(m[1].payload, {0x01}));
  return true;
}
static_assert(split());

// What a program in the RAM of the module prints is not mistaken for the module's firmware: single 0xC0 bytes, text,
// and a fixed pseudo-random binary stream with many 0xC0 bytes give no message, and a real frame in the middle of such
// noise is still found.
constexpr bool text_is_no_frame() {
  SlipDecoder d;
  Bytes text;
  for (int i = 0; i < 50; i++) {
    constexpr std::string_view line = "P 123 -61 ok 0A602C5123EB04CA7A8F93\r\n";
    text.insert(text.end(), line.begin(), line.end());
  }
  CHECK(feed(d, text).empty());
  CHECK(feed(d, {0xC0}).empty());
  CHECK(feed(d, {0xC0, 0xC0, 0xC0, 0x41, 0xC0}).empty());
  return true;
}
static_assert(text_is_no_frame());

// A fixed pseudo-random stream. A quarter of the bytes are the frame delimiter, to give the decoder many candidates.
struct Noise {
  uint32_t x = 2026;
  constexpr uint8_t next() {
    x = x * 1664525 + 1013904223;
    return (x >> 30) == 0 ? SLIP_END : uint8_t(x >> 16);
  }
};

constexpr Bytes noise(size_t n) {
  Noise g;
  Bytes out(n);
  for (auto &b : out)
    b = g.next();
  return out;
}

// straight into the decoder, which is cheaper to evaluate than a vector of it. About 2000 of the frames are long enough
// for their CRC to be checked: 20000 bytes keep this within the default limit of constexpr evaluation.
constexpr bool noise_is_no_frame() {
  SlipDecoder d;
  Noise g;
  for (int i = 0; i < 20000; i++)
    CHECK(!d.push(g.next()));
  return true;
}
static_assert(noise_is_no_frame());

constexpr bool frame_in_noise() {
  const Bytes n = noise(6000);
  Bytes mixed(n.begin(), n.begin() + 3000);
  for (const uint8_t b : enc(SAP_DEVICE_MANAGEMENT, DEVMGMT_PING_RSP, std::array<uint8_t, 1>{STATUS_OK}))
    mixed.push_back(b);
  mixed.insert(mixed.end(), n.begin() + 3000, n.end());
  SlipDecoder d;
  const auto found = feed(d, mixed);
  CHECK(found.size() == 1 && found[0].id == DEVMGMT_PING_RSP && same(found[0].payload, {STATUS_OK}));
  return true;
}
static_assert(frame_in_noise());

// errors: CRC, too short, a bad escape, a frame that is too long. None of them stops the next message.
constexpr bool errors() {
  SlipDecoder d;
  CHECK(feed(d, {0xC0, 0x01, 0x02, 0x00, 0xA0, 0xAE, 0xC0}).empty());
  CHECK(d.take_error() == SlipDecoder::Error::ERROR_CRC);
  CHECK(d.take_error() == SlipDecoder::Error::ERROR_NONE);
  CHECK(feed(d, {0xC0, 0x01, 0x02, 0xC0}).empty());
  CHECK(d.take_error() == SlipDecoder::Error::ERROR_TOO_SHORT);
  CHECK(feed(d, {0xC0, 0x01, 0xDB, 0x42, 0x00, 0x00, 0xC0}).empty());
  CHECK(d.take_error() == SlipDecoder::Error::ERROR_BAD_ESCAPE);
  Bytes long_frame{0xC0};
  long_frame.insert(long_frame.end(), MAX_PAYLOAD_SIZE + MESSAGE_OVERHEAD + 10, 0x11);
  long_frame.push_back(0xC0);
  CHECK(feed(d, long_frame).empty());
  CHECK(d.take_error() == SlipDecoder::Error::ERROR_OVERFLOW);
  CHECK(feed(d, {0xC0, 0x01, 0x02, 0x00, 0xA0, 0xAF, 0xC0}).size() == 1);
  return true;
}
static_assert(errors());

// the longest message there can be gets through, one byte more does not
constexpr bool longest() {
  const Bytes payload(MAX_PAYLOAD_SIZE, 0xC0);
  SlipDecoder d;
  const auto m = feed(d, enc(0x09, 0x55, payload));
  CHECK(m.size() == 1 && same(m[0].payload, payload));
  const Bytes more(MAX_PAYLOAD_SIZE + 1, 0x00);
  CHECK(feed(d, enc(0x09, 0x55, more)).empty());
  CHECK(d.take_error() == SlipDecoder::Error::ERROR_OVERFLOW);
  return true;
}
static_assert(longest());

// ---------------------------------------------------------------- configuration

// the configuration of a working receiver: C/T mode, a literal 0xC0 in the re-calibration timeout (600000 ms)
constexpr GatewayConfig working() {
  GatewayConfig c;
  c.link_mode = LINK_MODE_C_T;
  c.options = OPTION_RX_NOTIFICATION | OPTION_TX_NOTIFICATION | OPTION_RECALIBRATION;
  c.ui_options = 0x0005;
  c.led_flash_ms = 50;
  c.recalibration_timeout_ms = 600000;
  return c;
}
static_assert(same(enc(SAP_WMBUS_GATEWAY, WMBUS_SET_ACTIVE_CONFIG_REQ, working().encode()),
                   {0xC0, 0x09, 0x03, 0x03, 0x0E, 0x00, 0x05, 0x00, 0x32, 0x00, 0xDB, 0xDC, 0x27, 0x09, 0x00, 0x90,
                    0x24, 0xC0}));

constexpr bool config_round_trip() {
  const GatewayConfig c = working();
  SlipDecoder d;
  const auto m = feed(d, enc(SAP_WMBUS_GATEWAY, WMBUS_SET_ACTIVE_CONFIG_REQ, c.encode()));
  CHECK(m.size() == 1 && m[0].payload.size() == GatewayConfig::SIZE);
  GatewayConfig back;
  CHECK(GatewayConfig::decode(m[0].payload, back));
  CHECK(back.matches(c));
  CHECK(back.recalibration_timeout_ms == 600000);
  // an answer without the timeout is compared on the rest
  GatewayConfig old;
  CHECK(GatewayConfig::decode(std::span(m[0].payload).first(GatewayConfig::SIZE_WITHOUT_RECALIBRATION), old));
  CHECK(!old.has_recalibration_timeout && old.matches(c));
  GatewayConfig other = c;
  other.link_mode = LINK_MODE_S;
  CHECK(!back.matches(other));
  CHECK(!GatewayConfig::decode(std::span(m[0].payload).first(6), old));
  return true;
}
static_assert(config_round_trip());

// ---------------------------------------------------------------- received packets

constexpr std::array<uint8_t, 23> RX_PAYLOAD{0x19, 0x9E, 0x64, 0x5F,  // 1600429593
                                             0x00, 0x00, 0x05, 0xAF,  // not encrypted, no mode, C mode 100k A, -81 dBm
                                             0x0E, 0x44, 0x43, 0x04, 0x78, 0x56, 0x34, 0x12,
                                             0x01, 0x07, 0x7A, 0x01, 0x02, 0x03, 0x04};

constexpr WMBusPacket received() {
  SlipDecoder d;
  WMBusPacket p;
  for (const uint8_t b : enc(SAP_WMBUS_GATEWAY, WMBUS_RX_MESSAGE_IND, RX_PAYLOAD)) {
    if (d.push(b) && d.message().id == WMBUS_RX_MESSAGE_IND)
      parse_rx_message(d.message().payload, p);
  }
  return p;
}
constexpr WMBusPacket PACKET = received();
static_assert(PACKET.timestamp == 1600429593);
static_assert(PACKET.decryption_status == 0 && PACKET.encryption_mode == 0);
static_assert(PACKET.packet_info == 5);
static_assert(PACKET.rssi == -81);
static_assert(PACKET.size == 15 && PACKET.data().size() == 15 && PACKET.data()[14] == 0x04);
static_assert(PACKET.l_field() == 0x0E && PACKET.c_field() == 0x44);
static_assert(text(PACKET.manufacturer()) == "ABC");
static_assert(text(PACKET.device_id()) == "12345678");
static_assert(text(PACKET.meter_id()) == "ABC12345678");
static_assert(PACKET.version() == 1 && PACKET.device_type() == 7);

// too short to be a packet, or only a header
constexpr bool short_packets() {
  WMBusPacket p;
  CHECK(!parse_rx_message(std::span(RX_PAYLOAD).first(8), p));
  CHECK(parse_rx_message(std::span(RX_PAYLOAD).first(9), p));
  CHECK(!p.has_address() && text(p.meter_id()).empty() && text(p.manufacturer()).empty());
  return true;
}
static_assert(short_packets());

// the longest packet fits
constexpr bool longest_packet() {
  const Bytes payload(MAX_PAYLOAD_SIZE, 0x5A);
  WMBusPacket p;
  CHECK(parse_rx_message(payload, p) && p.data().size() == MAX_PACKET_SIZE);
  // a copy has its own bytes
  const WMBusPacket copy = p;
  p.buffer[0] = 0x00;
  CHECK(copy.data()[0] == 0x5A && copy.data().data() != p.data().data());
  return true;
}
static_assert(longest_packet());

// ---------------------------------------------------------------- device management answers

constexpr std::array<uint8_t, 13> DEV{109, 0x78, 0x56, 0x34, 0x12, 1, 2, 3, 4, 5, 6, 7, 8};
constexpr bool device_info() {
  DeviceInfo info;
  CHECK(DeviceInfo::decode(DEV, info));
  CHECK(info.module_type == MODULE_TYPE_IM891A_XL);
  CHECK(info.module_id == 0x12345678 && info.has_product && info.product_id[3] == 8);
  CHECK(DeviceInfo::decode(std::span(DEV).first(5), info) && !info.has_product);
  CHECK(!DeviceInfo::decode(std::span(DEV).first(4), info));
  return true;
}
static_assert(device_info());

// the example of the specification
constexpr std::array<uint8_t, 29> FW{0x09, 0x00, 0x37, 0x00, '0', '9', '.', '0', '4', '.', '2', '0', '2', '0', 'W',
                                     'M',  'B',  'u',  's',  '_', 'R', 'a', 'n', 'g', 'e', '_', 'E', 'x', 't'};
constexpr bool firmware_info() {
  FirmwareInfo fw;
  CHECK(FirmwareInfo::decode(FW, fw));
  CHECK(fw.major == 0 && fw.minor == 9 && fw.build_count == 55);
  CHECK(text(fw.build_date) == "09.04.2020" && text(fw.name) == "WMBus_Range_Ext");
  CHECK(!FirmwareInfo::decode(std::span(FW).first(13), fw));
  // a name longer than there is room for is cut off, and decoding again replaces it
  Bytes longer(FW.begin(), FW.end());
  longer.insert(longer.end(), 100, 'x');
  CHECK(FirmwareInfo::decode(longer, fw) && text(fw.name).size() == MAX_FIRMWARE_NAME);
  CHECK(FirmwareInfo::decode(FW, fw) && text(fw.name) == "WMBus_Range_Ext");
  return true;
}
static_assert(firmware_info());

}  // namespace

int main() { return 0; }

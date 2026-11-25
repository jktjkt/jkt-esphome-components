#pragma once

// WiMOD HCI protocol of the IMST WM-Bus Gateway firmware (iM891A-XL, iM881A-XL, iU891A-XL), see the "Wireless M-Bus
// Gateway Firmware Host Controller Interface Protocol" specification, version 2.4.
// Standard library only, so that it can be tested on the host, and constexpr, so that the tests are static_asserts.
// Nothing here allocates. The names of the codes are tables in the component.
//
// A message is <SAP ID> <message ID> <payload, 0 to 500 bytes> <CRC-16, LSB first>. On the UART it is sent as a SLIP
// frame: END (0xC0), the message with END and ESC escaped, END.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace esphome::im891a_xl::hci {

constexpr uint8_t SLIP_END = 0xC0;
constexpr uint8_t SLIP_ESC = 0xDB;
constexpr uint8_t SLIP_ESC_END = 0xDC;
constexpr uint8_t SLIP_ESC_ESC = 0xDD;

constexpr size_t MAX_PAYLOAD_SIZE = 500;
// SAP ID, message ID and the CRC
constexpr size_t MESSAGE_OVERHEAD = 4;

/// The longest SLIP frame of a message with this much payload: every byte escaped, and an END on both sides
constexpr size_t frame_capacity(size_t payload) { return 2 + 2 * (payload + MESSAGE_OVERHEAD); }

constexpr uint8_t SAP_DEVICE_MANAGEMENT = 0x01;
constexpr uint8_t SAP_WMBUS_GATEWAY = 0x09;

// Device management messages. A response has the ID of its request plus one.
constexpr uint8_t DEVMGMT_STARTUP_IND = 0x00;
constexpr uint8_t DEVMGMT_PING_REQ = 0x01;
constexpr uint8_t DEVMGMT_PING_RSP = 0x02;
constexpr uint8_t DEVMGMT_GET_DEVICE_INFO_REQ = 0x03;
constexpr uint8_t DEVMGMT_GET_DEVICE_INFO_RSP = 0x04;
constexpr uint8_t DEVMGMT_GET_FIRMWARE_INFO_REQ = 0x05;
constexpr uint8_t DEVMGMT_GET_FIRMWARE_INFO_RSP = 0x06;
// Not in the specification, but in IMST's example code: the module answers, and restarts into the ST ROM bootloader.
// The answer is C0 01 F2 00 A8 D3 C0. A RAM image that implements it also leaves for the bootloader.
constexpr uint8_t DEVMGMT_START_BOOTLOADER_REQ = 0xF1;
constexpr uint8_t DEVMGMT_START_BOOTLOADER_RSP = 0xF2;

// WM-Bus gateway messages
constexpr uint8_t WMBUS_GET_ACTIVE_CONFIG_REQ = 0x01;
constexpr uint8_t WMBUS_GET_ACTIVE_CONFIG_RSP = 0x02;
constexpr uint8_t WMBUS_SET_ACTIVE_CONFIG_REQ = 0x03;
constexpr uint8_t WMBUS_SET_ACTIVE_CONFIG_RSP = 0x04;
constexpr uint8_t WMBUS_RX_MESSAGE_IND = 0x20;
constexpr uint8_t WMBUS_GET_STATUS_REPORT_REQ = 0x41;
constexpr uint8_t WMBUS_GET_STATUS_REPORT_RSP = 0x42;

constexpr uint8_t STATUS_OK = 0x00;

/// CRC-16/IBM-SDLC (X-25): polynomial 0x1021 reflected, initial value 0xFFFF, final XOR 0xFFFF.
/// `crc` and `final` continue a CRC over pieces: the earlier ones without the final XOR.
/// esphome::crc16() computes the same, but this header stays free of ESPHome so that the tests run on the host.
constexpr uint16_t crc16(std::span<const uint8_t> data, uint16_t crc = 0xFFFF, bool final = true) {
  for (const uint8_t b : data) {
    crc ^= b;
    for (int bit = 0; bit < 8; bit++)
      crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
  }
  return final ? crc ^ 0xFFFF : crc;
}

/// A message as SlipDecoder delivers it. The payload points into the decoder, and is valid until it is fed again.
struct Message {
  uint8_t sap{0};
  uint8_t id{0};
  std::span<const uint8_t> payload;
};

/// Writes the complete SLIP frame of a message into `out`, ready for the UART.
/// Returns its length, or 0 if it does not fit: frame_capacity() of the payload always fits.
constexpr size_t encode(std::span<uint8_t> out, uint8_t sap, uint8_t id, std::span<const uint8_t> payload = {}) {
  size_t len = 0;
  bool fits = true;
  const auto put_raw = [&](uint8_t b) {
    if (len < out.size()) {
      out[len++] = b;
    } else {
      fits = false;
    }
  };
  const auto put = [&](uint8_t b) {
    if (b == SLIP_END) {
      put_raw(SLIP_ESC);
      put_raw(SLIP_ESC_END);
    } else if (b == SLIP_ESC) {
      put_raw(SLIP_ESC);
      put_raw(SLIP_ESC_ESC);
    } else {
      put_raw(b);
    }
  };
  const std::array<uint8_t, 2> header{sap, id};
  // the CRC over the header and the payload, as crc16() over both together
  const uint16_t crc = crc16(payload, crc16(header, 0xFFFF, false));
  put_raw(SLIP_END);
  for (const uint8_t b : header)
    put(b);
  for (const uint8_t b : payload)
    put(b);
  put(crc & 0xFF);
  put(crc >> 8);
  put_raw(SLIP_END);
  return fits ? len : 0;
}

/// Splits the UART byte stream into messages. Feed it every received byte.
class SlipDecoder {
 public:
  enum class Error : uint8_t { ERROR_NONE, ERROR_TOO_SHORT, ERROR_CRC, ERROR_OVERFLOW, ERROR_BAD_ESCAPE };

  /// True when a complete message with a good CRC has arrived, it is in message() until the next push().
  constexpr bool push(uint8_t byte) {
    if (this->escaping_) {
      this->escaping_ = false;
      if (byte == SLIP_ESC_END) {
        this->append_(SLIP_END);
      } else if (byte == SLIP_ESC_ESC) {
        this->append_(SLIP_ESC);
      } else {
        this->fail_(Error::ERROR_BAD_ESCAPE);
      }
      return false;
    }
    if (byte == SLIP_ESC) {
      this->escaping_ = true;
      return false;
    }
    if (byte != SLIP_END) {
      this->append_(byte);
      return false;
    }
    // END: the end of a message, or the start of the next one
    const bool discarding = this->discarding_;
    this->discarding_ = false;
    const bool ok = !discarding && this->len_ > 0 && this->finish_();
    this->len_ = 0;
    return ok;
  }

  constexpr const Message &message() const { return this->message_; }

  /// Forgets the frame in progress, as if nothing had arrived yet
  constexpr void reset() { *this = SlipDecoder(); }

  /// The reason the last frame was dropped, cleared by reading it.
  constexpr Error take_error() {
    const Error e = this->error_;
    this->error_ = Error::ERROR_NONE;
    return e;
  }

 protected:
  constexpr void append_(uint8_t byte) {
    if (this->discarding_)
      return;
    if (this->len_ == this->buf_.size()) {
      this->fail_(Error::ERROR_OVERFLOW);
      return;
    }
    this->buf_[this->len_++] = byte;
  }

  // Drops the frame in progress, and everything up to the next END
  constexpr void fail_(Error e) {
    this->error_ = e;
    this->len_ = 0;
    this->discarding_ = true;
  }

  constexpr bool finish_() {
    const size_t n = this->len_;
    if (n < MESSAGE_OVERHEAD) {
      this->error_ = Error::ERROR_TOO_SHORT;
      return false;
    }
    const std::span<const uint8_t> body(this->buf_.data(), n);
    if (crc16(body.first(n - 2)) != (uint16_t(body[n - 1]) << 8 | body[n - 2])) {
      this->error_ = Error::ERROR_CRC;
      return false;
    }
    // The bytes stay in the buffer until the next one arrives
    this->message_.sap = body[0];
    this->message_.id = body[1];
    this->message_.payload = body.subspan(2, n - MESSAGE_OVERHEAD);
    return true;
  }

  std::array<uint8_t, MAX_PAYLOAD_SIZE + MESSAGE_OVERHEAD> buf_{};
  Message message_;
  uint16_t len_{0};
  Error error_{Error::ERROR_NONE};
  bool escaping_{false};
  bool discarding_{false};
};

// ---------------------------------------------------------------------------------------------------------------
// WM-Bus gateway configuration
// ---------------------------------------------------------------------------------------------------------------

enum LinkMode : uint8_t {
  LINK_MODE_OFF = 0,
  LINK_MODE_S = 1,
  LINK_MODE_T = 2,
  LINK_MODE_C_T = 3,  // T and C mode combined
  LINK_MODE_C = 5,    // C mode, 100 kbit/s
  LINK_MODE_ENHANCED_T = 6,
  LINK_MODE_CUSTOM_C_T = 7,
};

constexpr uint16_t OPTION_ADDRESS_FILTER = 0x0001;
constexpr uint16_t OPTION_RX_NOTIFICATION = 0x0002;  // received packets are sent to the host
constexpr uint16_t OPTION_TX_NOTIFICATION = 0x0004;
constexpr uint16_t OPTION_RECALIBRATION = 0x0008;

/// Set Active Configuration and Get Active Configuration carry the same 11 bytes.
struct GatewayConfig {
  uint32_t recalibration_timeout_ms{0};
  uint16_t options{0};
  uint16_t ui_options{0};
  uint16_t led_flash_ms{50};
  uint8_t link_mode{LINK_MODE_C_T};
  /// The re-calibration timeout is missing in the answer of older firmware.
  bool has_recalibration_timeout{true};

  static constexpr size_t SIZE = 11;
  static constexpr size_t SIZE_WITHOUT_RECALIBRATION = 7;

  constexpr std::array<uint8_t, SIZE> encode() const {
    return {link_mode,
            uint8_t(options),
            uint8_t(options >> 8),
            uint8_t(ui_options),
            uint8_t(ui_options >> 8),
            uint8_t(led_flash_ms),
            uint8_t(led_flash_ms >> 8),
            uint8_t(recalibration_timeout_ms),
            uint8_t(recalibration_timeout_ms >> 8),
            uint8_t(recalibration_timeout_ms >> 16),
            uint8_t(recalibration_timeout_ms >> 24)};
  }

  static constexpr bool decode(std::span<const uint8_t> data, GatewayConfig &out) {
    const size_t len = data.size();
    if (len < SIZE_WITHOUT_RECALIBRATION)
      return false;
    out.link_mode = data[0];
    out.options = uint16_t(data[2]) << 8 | data[1];
    out.ui_options = uint16_t(data[4]) << 8 | data[3];
    out.led_flash_ms = uint16_t(data[6]) << 8 | data[5];
    out.has_recalibration_timeout = len >= SIZE;
    out.recalibration_timeout_ms = out.has_recalibration_timeout ? uint32_t(data[10]) << 24 | uint32_t(data[9]) << 16 |
                                                                       uint32_t(data[8]) << 8 | data[7]
                                                                 : 0;
    return true;
  }

  /// Whether the module runs what was asked for. A module that does not report the timeout is not compared on it.
  constexpr bool matches(const GatewayConfig &wanted) const {
    return link_mode == wanted.link_mode && options == wanted.options && ui_options == wanted.ui_options &&
           led_flash_ms == wanted.led_flash_ms &&
           (!has_recalibration_timeout || recalibration_timeout_ms == wanted.recalibration_timeout_ms);
  }
};

/// Copies bytes into a NUL terminated text of a fixed size, cutting off what does not fit.
template<size_t N> constexpr void copy_text(std::array<char, N> &out, std::span<const uint8_t> in) {
  const size_t n = std::min(in.size(), N - 1);
  std::copy_n(in.begin(), n, out.begin());
  out[n] = '\0';
}

// ---------------------------------------------------------------------------------------------------------------
// Received WM-Bus packets
// ---------------------------------------------------------------------------------------------------------------

// The header of a WM-Bus Rx Message indication: timestamp (4), decryption status, encryption mode, packet info, RSSI
constexpr size_t RX_MESSAGE_HEADER = 8;
constexpr size_t MAX_PACKET_SIZE = MAX_PAYLOAD_SIZE - RX_MESSAGE_HEADER;

/// WM-Bus Rx Message indication: what the module received, with the physical layer CRCs removed.
/// It holds its bytes, so a copy of it stays valid.
struct WMBusPacket {
  uint32_t timestamp{0};  // UTC seconds of the module's clock
  uint16_t size{0};       // bytes in buffer
  uint8_t decryption_status{0};
  uint8_t encryption_mode{0};
  uint8_t packet_info{0};  // link mode and frame format the packet came in
  int8_t rssi{0};          // dBm
  std::array<uint8_t, MAX_PACKET_SIZE> buffer{};

  /// L-field, C-field, manufacturer (2), device ID (4), version, type, then the rest. For frame format B the L-field is
  /// corrected for the removed CRCs.
  constexpr std::span<const uint8_t> data() const { return {this->buffer.data(), this->size}; }

  constexpr uint8_t l_field() const { return this->size > 0 ? this->buffer[0] : 0; }
  constexpr uint8_t c_field() const { return this->size > 1 ? this->buffer[1] : 0; }
  constexpr bool has_address() const { return this->size >= 10; }
  constexpr uint16_t manufacturer_code() const {
    return this->size > 3 ? uint16_t(this->buffer[3]) << 8 | this->buffer[2] : 0;
  }
  /// The three letters of the manufacturer code, e.g. KAM, NUL terminated. Empty if the packet is too short.
  constexpr std::array<char, 4> manufacturer() const {
    std::array<char, 4> s{};
    if (this->size < 4)
      return s;
    const uint16_t m = this->manufacturer_code();
    s[0] = char((m / 1024) % 32 + 64);
    s[1] = char((m / 32) % 32 + 64);
    s[2] = char(m % 32 + 64);
    return s;
  }
  /// The device ID as its 8 BCD digits, most significant first (it is sent least significant byte first), NUL
  /// terminated. Empty if the packet is too short.
  constexpr std::array<char, 9> device_id() const {
    std::array<char, 9> s{};
    if (this->size < 8)
      return s;
    for (size_t i = 0; i < 4; i++) {
      const uint8_t b = this->buffer[7 - i];
      s[2 * i] = char((b >> 4) + '0');
      s[2 * i + 1] = char((b & 0x0F) + '0');
    }
    return s;
  }
  /// Manufacturer and device ID together, as the meter is usually named, NUL terminated
  constexpr std::array<char, 12> meter_id() const {
    std::array<char, 12> s{};
    const auto m = this->manufacturer();
    const auto d = this->device_id();
    if (m[0] == '\0' || d[0] == '\0')
      return s;
    std::copy_n(m.begin(), 3, s.begin());
    std::copy_n(d.begin(), 8, s.begin() + 3);
    return s;
  }
  constexpr uint8_t version() const { return this->size > 8 ? this->buffer[8] : 0; }
  constexpr uint8_t device_type() const { return this->size > 9 ? this->buffer[9] : 0; }
};

/// Parses the payload of a WM-Bus Rx Message indication.
constexpr bool parse_rx_message(std::span<const uint8_t> payload, WMBusPacket &out) {
  if (payload.size() < RX_MESSAGE_HEADER + 1 || payload.size() > MAX_PAYLOAD_SIZE)
    return false;
  out.timestamp = uint32_t(payload[3]) << 24 | uint32_t(payload[2]) << 16 | uint32_t(payload[1]) << 8 | payload[0];
  out.decryption_status = payload[4];
  out.encryption_mode = payload[5];
  out.packet_info = payload[6];
  out.rssi = static_cast<int8_t>(payload[7]);
  const auto data = payload.subspan(RX_MESSAGE_HEADER);
  std::copy(data.begin(), data.end(), out.buffer.begin());
  out.size = uint16_t(data.size());
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Device management answers
// ---------------------------------------------------------------------------------------------------------------

constexpr uint8_t MODULE_TYPE_IM891A_XL = 109;

struct DeviceInfo {
  uint32_t module_id{0};
  uint8_t module_type{0};  // 0 until the module told it
  bool has_product{false};
  uint8_t product_type[4]{};
  uint8_t product_id[4]{};

  /// The payload after the status byte
  static constexpr bool decode(std::span<const uint8_t> data, DeviceInfo &out) {
    const size_t len = data.size();
    if (len < 5)
      return false;
    out.module_type = data[0];
    out.module_id = uint32_t(data[4]) << 24 | uint32_t(data[3]) << 16 | uint32_t(data[2]) << 8 | data[1];
    out.has_product = len >= 13;
    if (out.has_product) {
      std::copy_n(data.begin() + 5, 4, out.product_type);
      std::copy_n(data.begin() + 9, 4, out.product_id);
    }
    return true;
  }
};

/// A longer firmware name is cut off. IMST's names are like "WMBus_Range_Ext".
constexpr size_t MAX_FIRMWARE_NAME = 64;

struct FirmwareInfo {
  uint16_t build_count{0};
  uint8_t major{0};
  uint8_t minor{0};
  std::array<char, 11> build_date{};  // "dd.mm.yyyy", empty until the module told it
  std::array<char, MAX_FIRMWARE_NAME + 1> name{};

  /// The payload after the status byte. The version is sent minor first.
  static constexpr bool decode(std::span<const uint8_t> data, FirmwareInfo &out) {
    if (data.size() < 14)
      return false;
    out.minor = data[0];
    out.major = data[1];
    out.build_count = uint16_t(data[3]) << 8 | data[2];
    copy_text(out.build_date, data.subspan(4, 10));
    copy_text(out.name, data.subspan(14));
    return true;
  }
};

}  // namespace esphome::im891a_xl::hci

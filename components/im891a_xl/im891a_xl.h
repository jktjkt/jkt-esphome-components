#pragma once

#include "bootloader.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/line_reader.h"
#include "esphome/core/log.h"
#include "esphome/core/string_ref.h"
#include "hci.h"

#include <array>
#include <span>
#include <string_view>
#include <utility>

namespace esphome::im891a_xl {

/// A program for the RAM of the module, compiled into the firmware. The code generator puts them all into one table.
struct RamImage {
  const char *name;
  const uint8_t *data;
  size_t size;
  uint32_t address;  // where it is written and started
  bool verify;       // read it back before starting it
};

/// A line of the RAM program that is longer than this is passed on in pieces
static constexpr size_t MAX_IMAGE_LINE = 512;

/// The link mode and frame format of a received packet, from hci::WMBusPacket::packet_info
const LogString *packet_info_to_str(uint8_t packet_info);

/// Driver for the IMST iM891A-XL (also iM881A-XL and iU891A-XL) running the WM-Bus Gateway firmware.
/// It sets the module up for receiving, keeps it configured, and hands every received WM-Bus packet to its callbacks.
///
/// The module can also be restarted into the ST ROM bootloader of its STM32WLE5, and a program be loaded into its RAM
/// and started. The UART then changes format twice, and the HCI protocol is not spoken meanwhile:
///   HCI (115200 8N1, SLIP)  ->  bootloader (8E1, AN3155)  ->  the program (its own format, 8N1 by default)
/// A reset brings the module's firmware back.
class IM891AXL final : public Component, public uart::UARTDevice {
 public:
  void set_reset_pin(GPIOPin *pin) { this->reset_pin_ = pin; }
  void set_link_mode(uint8_t mode) { this->wanted_.link_mode = mode; }
  void set_recalibration_timeout(uint32_t ms) { this->wanted_.recalibration_timeout_ms = ms; }
  void set_health_check_interval(uint32_t ms) { this->health_check_interval_ms_ = ms; }
  void set_image_parity(uart::UARTParityOptions parity) { this->image_parity_ = parity; }
  void set_image_detect_restart(bool detect) { this->image_detect_restart_ = detect; }
  void set_ram_images(const RamImage *images, size_t count) { this->images_ = {images, count}; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  /// Every received packet: the WM-Bus frame without the physical layer CRCs, with RSSI and the mode it came in
  template<typename F> void add_on_packet_callback(F &&callback) {
    this->packet_callback_.add(std::forward<F>(callback));
  }
  /// Every line the running RAM program prints, valid while the callback runs. Bytes that are not printable show as
  /// dots.
  template<typename F> void add_on_image_line_callback(F &&callback) {
    this->image_line_callback_.add(std::forward<F>(callback));
  }

  /// True once the module answers and runs the wanted configuration
  bool is_ready() const { return this->state_ == State::STATE_RUNNING || this->state_ == State::STATE_CHECK_CONFIG; }
  bool is_in_bootloader() const {
    return this->state_ == State::STATE_BOOTLOADER || this->state_ == State::STATE_BOOTLOADER_WAIT;
  }
  bool is_image_running() const { return this->state_ == State::STATE_IMAGE_RUNNING; }
  uint32_t get_packet_count() const { return this->packet_count_; }

  /// Restarts the module into the ROM bootloader. By HCI request from the module's firmware, or from a running RAM
  /// image, or, with `via_hci` false, not at all: the module is in the bootloader already (BOOT0 held at a reset).
  bool start_bootloader(bool via_hci = true);
  /// Writes the image to the RAM, reads it back and starts it. Restarts the module into the bootloader first if it is
  /// not there. False if there is no such image, or the module cannot do it now.
  bool load_image(std::string_view name);
  /// Resets the module, which brings its own firmware back whatever it was doing
  void reset();

 protected:
  enum class State : uint8_t {
    STATE_WAIT_TO_RESET,  // a pause before the module is reset, after a failure
    STATE_BOOTING,        // reset done, or no reset pin, waiting for the module to start
    STATE_PING,           // the steps of the start-up, each waits for the answer to its request
    STATE_DEVICE_INFO,
    STATE_FIRMWARE_INFO,
    STATE_SET_CONFIG,
    STATE_VERIFY_CONFIG,
    STATE_RUNNING,
    STATE_CHECK_CONFIG,        // the periodic check that the module still has the configuration
    STATE_BOOTLOADER_REQUEST,  // StartBootloader sent, waiting for the answer
    STATE_BOOTLOADER_WAIT,     // the module restarts into the bootloader, the UART is in the bootloader's format
    STATE_BOOTLOADER,          // talking AN3155 to the bootloader
    STATE_IMAGE_RUNNING,       // the RAM program runs, and what it prints is forwarded
  };

  /// The longest request this driver sends is Set Active Configuration
  static constexpr size_t REQUEST_FRAME_SIZE = hci::frame_capacity(hci::GatewayConfig::SIZE);

  /// A request whose frame does not fit REQUEST_FRAME_SIZE does not compile
  template<size_t P = 0>
  void send_request_(State state, uint8_t sap, uint8_t id, const std::array<uint8_t, P> &payload = {}) {
    static_assert(hci::frame_capacity(P) <= REQUEST_FRAME_SIZE, "request too long for the frame buffer");
    this->pending_frame_len_ = hci::encode(this->pending_frame_, sap, id, payload);
    this->start_request_(state, sap, id);
  }
  void start_request_(State state, uint8_t sap, uint8_t id);
  void resend_request_();
  void start_reset_(uint32_t delay_ms);
  void start_configuring_();
  void fail_(const LogString *what);
  bool config_matches_(const hci::Message &message);

  void read_uart_();
  void on_message_(const hci::Message &message);
  void on_response_(const hci::Message &message);
  void on_rx_message_(const hci::Message &message);

  void loop_bootloader_(uint32_t now);
  void loop_image_();
  bool on_image_byte_(uint8_t byte);
  void publish_image_line_(const char *line, size_t len);
  void enter_bootloader_wait_();
  void start_loading_(const RamImage &image, uint32_t now);
  void leave_bootloader_failed_();
  void enter_image_mode_();
  void leave_image_mode_();
  void set_uart_parity_(uart::UARTParityOptions parity);
  const RamImage *find_image_(std::string_view name) const;

  hci::SlipDecoder decoder_;
  bootloader::Client bootloader_;
  LineReader<MAX_IMAGE_LINE + 1> image_line_;
  hci::FirmwareInfo firmware_info_;
  hci::DeviceInfo device_info_;
  hci::GatewayConfig wanted_;
  std::span<const RamImage> images_;
  LazyCallbackManager<void(const hci::WMBusPacket &)> packet_callback_;
  LazyCallbackManager<void(StringRef)> image_line_callback_;
  GPIOPin *reset_pin_{nullptr};
  const RamImage *pending_image_{nullptr};  // to load as soon as the bootloader is ready
  const RamImage *loading_image_{nullptr};
  uint32_t health_check_interval_ms_{30000};
  uint32_t state_since_{0};
  uint32_t pending_since_{0};  // when the request that waits for its answer was sent
  uint32_t last_check_{0};
  uint32_t packet_count_{0};
  std::array<uint8_t, REQUEST_FRAME_SIZE> pending_frame_{};
  uint8_t pending_frame_len_{0};
  uint8_t pending_sap_{0};
  uint8_t pending_id_{0};
  uint8_t pending_tries_{0};
  uint8_t last_progress_{0};
  State state_{State::STATE_PING};
  uart::UARTParityOptions image_parity_{uart::UART_CONFIG_PARITY_NONE};
  bool pending_{false};
  bool image_detect_restart_{true};
  bool bootloader_ready_logged_{false};
};

}  // namespace esphome::im891a_xl

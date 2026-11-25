#include "im891a_xl.h"
#include "esphome/core/application.h"
#include "esphome/core/progmem.h"

#include <cinttypes>

namespace esphome::im891a_xl {

ESPHOME_LOG_TAG(TAG, "im891a_xl");

// The module answers within a second, and needs a moment after a reset before it listens
static constexpr uint32_t RESPONSE_TIMEOUT_MS = 1000;
static constexpr uint8_t REQUEST_TRIES = 3;
static constexpr uint32_t RESET_PULSE_MS = 10;
static constexpr uint32_t BOOT_TIME_MS = 500;
static constexpr uint32_t RETRY_DELAY_MS = 5000;
// A WM-Bus frame is at most 256 bytes
static constexpr size_t MAX_LOGGED_BYTES = 256;
// The module resets itself after answering StartBootloader, and the ROM bootloader needs a moment to start
static constexpr uint32_t BOOTLOADER_START_MS = 100;

// Status byte of an answer, indexed by its value
PROGMEM_STRING_TABLE(StatusStrings, "ok", "error", "command not supported", "wrong parameter",
                     "wrong application / device mode", "reserved", "busy, try later", "wrong message length",
                     "NVM write error", "NVM read error", "command rejected", "reserved", "unexpected message format",
                     "reserved");
// hci::LinkMode
PROGMEM_STRING_TABLE(LinkModeStrings, "off", "S", "T", "C/T", "unknown", "C", "enhanced T", "custom C/T", "unknown");
// hci::WMBusPacket::packet_info
PROGMEM_STRING_TABLE(PacketInfoStrings, "unknown", "S", "T", "unknown", "C 50k A", "C 100k A", "enhanced T", "custom T",
                     "custom C 100k A", "unknown", "unknown", "unknown", "unknown", "unknown", "unknown", "unknown",
                     "unknown", "unknown", "unknown", "unknown", "C 50k B", "C 100k B", "unknown", "unknown",
                     "custom C 100k B", "unknown");
// hci::WMBusPacket::decryption_status
PROGMEM_STRING_TABLE(DecryptionStatusStrings, "not encrypted", "decrypted", "no key", "mode not supported",
                     "decryption error", "unknown");
// in the order of MODULE_TYPES below
PROGMEM_STRING_TABLE(ModuleTypeStrings, "iM891A-XL", "iU891A-XL", "iM881A-XL", "unknown");
// hci::SlipDecoder::Error
PROGMEM_STRING_TABLE(SlipErrorStrings, "none", "frame too short", "CRC error", "frame too long", "bad SLIP escape");
// bootloader::Client::State
PROGMEM_STRING_TABLE(BootloaderStateStrings, "idle", "sync", "Get", "Get", "Get", "Get", "Get ID", "Get ID", "Get ID",
                     "Get ID", "ready", "Write Memory", "Write Memory", "Write Memory", "Read Memory", "Read Memory",
                     "Read Memory", "Read Memory", "Go", "Go", "done", "failed");
// bootloader::Client::Error
PROGMEM_STRING_TABLE(BootloaderErrorStrings, "none", "not ready to load", "empty image",
                     "a command the image needs is missing", "too much to send", "no answer to the sync byte",
                     "no answer", "NACK", "unexpected byte", "wrong product ID", "verify failed", "unknown");

const LogString *packet_info_to_str(uint8_t packet_info) { return PacketInfoStrings::get_log_str(packet_info); }

static const LogString *module_type_to_str(uint8_t type) {
  static constexpr uint8_t MODULE_TYPES[] = {hci::MODULE_TYPE_IM891A_XL, 110, 163};
  uint8_t i = 0;
  while (i < sizeof(MODULE_TYPES) && MODULE_TYPES[i] != type)
    i++;
  return ModuleTypeStrings::get_log_str(i);
}

void IM891AXL::setup() {
  // The module keeps what the last run set up, so every run starts with the configuration it wants. It is only
  // written to the RAM of the module, and the periodic check applies it again if the module restarts.
  this->wanted_.options = hci::OPTION_RX_NOTIFICATION | hci::OPTION_RECALIBRATION;
  this->wanted_.ui_options = 0;
  this->wanted_.led_flash_ms = 50;
  setup_output_pin(this->reset_pin_, true);
  this->start_reset_(0);
}

void IM891AXL::dump_config() {
  ESP_LOGCONFIG(TAG,
                "IM891A-XL WM-Bus receiver:\n"
                "  Link mode: %s\n"
                "  Re-calibration timeout: %" PRIu32 " ms\n"
                "  Configuration check every: %" PRIu32 " ms",
                LOG_STR_ARG(LinkModeStrings::get_log_str(this->wanted_.link_mode)),
                this->wanted_.recalibration_timeout_ms, this->health_check_interval_ms_);
  LOG_PIN("  Reset pin: ", this->reset_pin_);
  if (this->device_info_.module_type != 0) {
    ESP_LOGCONFIG(TAG, "  Module: %s (type %u), ID %08" PRIX32,
                  LOG_STR_ARG(module_type_to_str(this->device_info_.module_type)), this->device_info_.module_type,
                  this->device_info_.module_id);
  }
  if (this->firmware_info_.build_date[0] != '\0') {
    ESP_LOGCONFIG(TAG, "  Firmware: %s %u.%u, build %u of %s", this->firmware_info_.name.data(),
                  this->firmware_info_.major, this->firmware_info_.minor, this->firmware_info_.build_count,
                  this->firmware_info_.build_date.data());
  }
  for (const auto &image : this->images_) {
    ESP_LOGCONFIG(TAG, "  RAM image %s: %u bytes at 0x%08" PRIX32 "%s", image.name, (unsigned) image.size,
                  image.address, image.verify ? LOG_STR_LITERAL(", verified") : LOG_STR_LITERAL(""));
  }
  if (!this->images_.empty()) {
    ESP_LOGCONFIG(TAG,
                  "  UART parity of a running RAM image: %s\n"
                  "  Restart of the module detected from its output: %s",
                  LOG_STR_ARG(uart::parity_to_str(this->image_parity_)), YESNO(this->image_detect_restart_));
  }
  if (this->is_in_bootloader()) {
    ESP_LOGCONFIG(TAG, "  In the ROM bootloader");
  } else if (this->is_image_running()) {
    ESP_LOGCONFIG(TAG, "  Running a RAM image");
  } else if (!this->is_ready()) {
    ESP_LOGCONFIG(TAG, "  Not ready, the module has not answered yet");
  }
}

void IM891AXL::start_reset_(uint32_t delay_ms) {
  this->pending_ = false;
  this->decoder_.reset();
  this->state_ = State::STATE_WAIT_TO_RESET;
  // the time the reset is due, compared as a signed difference
  this->state_since_ = App.get_loop_component_start_time() + delay_ms;
}

void IM891AXL::start_configuring_() {
  this->send_request_(State::STATE_SET_CONFIG, hci::SAP_WMBUS_GATEWAY, hci::WMBUS_SET_ACTIVE_CONFIG_REQ,
                      this->wanted_.encode());
}

void IM891AXL::fail_(const LogString *what) {
  ESP_LOGW(TAG, "%s, starting over in %" PRIu32 " s", LOG_STR_ARG(what), RETRY_DELAY_MS / 1000);
  this->status_set_warning();
  this->start_reset_(RETRY_DELAY_MS);
}

void IM891AXL::start_request_(State state, uint8_t sap, uint8_t id) {
  this->state_ = state;
  this->pending_ = true;
  this->pending_sap_ = sap;
  this->pending_id_ = id;
  this->pending_tries_ = 0;
  this->resend_request_();
}

void IM891AXL::resend_request_() {
  ESP_LOGV(TAG, "TX SAP %02X message %02X, try %u", this->pending_sap_, this->pending_id_, this->pending_tries_ + 1);
  this->write_array(this->pending_frame_.data(), this->pending_frame_len_);
  this->pending_tries_++;
  this->pending_since_ = App.get_loop_component_start_time();
}

void IM891AXL::loop() {
  const uint32_t now = App.get_loop_component_start_time();
  // The HCI protocol is not spoken in these three
  switch (this->state_) {
    case State::STATE_BOOTLOADER:
      this->loop_bootloader_(now);
      return;
    case State::STATE_IMAGE_RUNNING:
      this->loop_image_();
      return;
    case State::STATE_BOOTLOADER_WAIT:
      if (now - this->state_since_ >= BOOTLOADER_START_MS) {
        this->discard_available();
        this->bootloader_.begin(bootloader::DEVICE_ID_STM32WLE5, now);
        this->bootloader_ready_logged_ = false;
        this->state_ = State::STATE_BOOTLOADER;
      }
      return;
    default:
      break;
  }
  this->read_uart_();

  switch (this->state_) {
    case State::STATE_WAIT_TO_RESET:
      if (int32_t(now - this->state_since_) >= 0) {
        pulse_reset(this->reset_pin_, {.low_ms = RESET_PULSE_MS});
        this->state_ = State::STATE_BOOTING;
        this->state_since_ = now;
      }
      break;
    case State::STATE_BOOTING:
      if (now - this->state_since_ >= BOOT_TIME_MS) {
        this->send_request_(State::STATE_PING, hci::SAP_DEVICE_MANAGEMENT, hci::DEVMGMT_PING_REQ);
      }
      break;
    case State::STATE_RUNNING:
      if (now - this->last_check_ >= this->health_check_interval_ms_) {
        this->last_check_ = now;
        this->send_request_(State::STATE_CHECK_CONFIG, hci::SAP_WMBUS_GATEWAY, hci::WMBUS_GET_ACTIVE_CONFIG_REQ);
      }
      break;
    default:
      break;
  }

  if (this->pending_ && now - this->pending_since_ >= RESPONSE_TIMEOUT_MS) {
    if (this->pending_tries_ < REQUEST_TRIES) {
      ESP_LOGD(TAG, "No answer to SAP %02X message %02X, asking again", this->pending_sap_, this->pending_id_);
      this->resend_request_();
    } else {
      this->fail_(LOG_STR("The module does not answer"));
    }
  }
}

void IM891AXL::read_uart_() {
  this->read_available([this](uint8_t byte) {
    if (this->decoder_.push(byte)) {
      this->on_message_(this->decoder_.message());
      // the rest is in the format of the bootloader
      return this->state_ != State::STATE_BOOTLOADER_WAIT;
    }
    const auto error = this->decoder_.take_error();
    if (error != hci::SlipDecoder::Error::ERROR_NONE) {
      // A running RAM image prints text, which is no frame, until the answer of the module arrives
      if (this->state_ == State::STATE_BOOTLOADER_REQUEST) {
        ESP_LOGD(TAG, "Dropped a frame from the module: %s", LOG_STR_ARG(SlipErrorStrings::get_log_str(error)));
      } else {
        ESP_LOGW(TAG, "Dropped a frame from the module: %s", LOG_STR_ARG(SlipErrorStrings::get_log_str(error)));
      }
    }
    return true;
  });
}

void IM891AXL::on_message_(const hci::Message &message) {
  if (message.sap == hci::SAP_WMBUS_GATEWAY && message.id == hci::WMBUS_RX_MESSAGE_IND) {
    this->on_rx_message_(message);
  } else if (message.sap == hci::SAP_DEVICE_MANAGEMENT && message.id == hci::DEVMGMT_STARTUP_IND) {
    // The module restarted on its own, so it lost the configuration
    ESP_LOGD(TAG, "The module announced a restart");
    if (this->is_ready()) {
      this->start_configuring_();
    }
  } else {
    this->on_response_(message);
  }
}

void IM891AXL::on_rx_message_(const hci::Message &message) {
  hci::WMBusPacket packet;
  if (!hci::parse_rx_message(message.payload, packet)) {
    ESP_LOGW(TAG, "Received packet message of %u bytes is too short", (unsigned) message.payload.size());
    return;
  }
  this->packet_count_++;
  ESP_LOGV_HEX(TAG, packet.buffer.data(), std::min<size_t>(packet.size, MAX_LOGGED_BYTES), MAX_LOGGED_BYTES,
               "%d dBm  %s  %s  [%s]", packet.rssi, LOG_STR_ARG(packet_info_to_str(packet.packet_info)),
               packet.meter_id().data());
  if (packet.decryption_status != 0 || packet.encryption_mode != 0) {
    ESP_LOGV(TAG, "Encryption mode %u, %s", packet.encryption_mode,
             LOG_STR_ARG(DecryptionStatusStrings::get_log_str(packet.decryption_status)));
  }
  this->packet_callback_.call(packet);
}

void IM891AXL::on_response_(const hci::Message &message) {
  if (!this->pending_ || message.sap != this->pending_sap_ || message.id != this->pending_id_ + 1) {
    ESP_LOGW(TAG, "Unexpected message from the module: SAP %02X message %02X", message.sap, message.id);
    return;
  }
  this->pending_ = false;
  if (message.payload.empty()) {
    this->fail_(LOG_STR("Empty answer from the module"));
    return;
  }
  const uint8_t status = message.payload[0];
  if (status != hci::STATUS_OK) {
    ESP_LOGE(TAG, "The module refused SAP %02X message %02X: %s (%02X)", message.sap, message.id - 1,
             LOG_STR_ARG(StatusStrings::get_log_str(status)), status);
    this->fail_(LOG_STR("Request refused"));
    return;
  }
  const auto data = message.payload.subspan(1);

  switch (this->state_) {
    case State::STATE_PING:
      this->send_request_(State::STATE_DEVICE_INFO, hci::SAP_DEVICE_MANAGEMENT, hci::DEVMGMT_GET_DEVICE_INFO_REQ);
      break;
    case State::STATE_DEVICE_INFO:
      if (hci::DeviceInfo::decode(data, this->device_info_)) {
        ESP_LOGD(TAG, "Module %s, ID %08" PRIX32, LOG_STR_ARG(module_type_to_str(this->device_info_.module_type)),
                 this->device_info_.module_id);
        if (this->device_info_.module_type != hci::MODULE_TYPE_IM891A_XL) {
          ESP_LOGW(TAG, "This driver was written for the iM891A-XL, not for module type %u",
                   this->device_info_.module_type);
        }
      }
      this->send_request_(State::STATE_FIRMWARE_INFO, hci::SAP_DEVICE_MANAGEMENT, hci::DEVMGMT_GET_FIRMWARE_INFO_REQ);
      break;
    case State::STATE_FIRMWARE_INFO:
      if (hci::FirmwareInfo::decode(data, this->firmware_info_)) {
        ESP_LOGD(TAG, "Firmware %s %u.%u, build %u of %s", this->firmware_info_.name.data(), this->firmware_info_.major,
                 this->firmware_info_.minor, this->firmware_info_.build_count, this->firmware_info_.build_date.data());
      }
      this->start_configuring_();
      break;
    case State::STATE_BOOTLOADER_REQUEST:
      ESP_LOGI(TAG, "The module is restarting into its ROM bootloader");
      this->enter_bootloader_wait_();
      break;
    case State::STATE_SET_CONFIG:
      // Read it back, to see what the module made of it
      this->send_request_(State::STATE_VERIFY_CONFIG, hci::SAP_WMBUS_GATEWAY, hci::WMBUS_GET_ACTIVE_CONFIG_REQ);
      break;
    case State::STATE_VERIFY_CONFIG:
      if (!this->config_matches_(message)) {
        this->fail_(LOG_STR("The module did not take the configuration"));
        break;
      }
      ESP_LOGI(TAG, "Receiving in link mode %s", LOG_STR_ARG(LinkModeStrings::get_log_str(this->wanted_.link_mode)));
      this->state_ = State::STATE_RUNNING;
      this->last_check_ = App.get_loop_component_start_time();
      this->status_clear_warning();
      break;
    case State::STATE_CHECK_CONFIG:
      if (this->config_matches_(message)) {
        this->state_ = State::STATE_RUNNING;
      } else {
        ESP_LOGW(TAG, "The module lost its configuration, setting it again");
        this->start_configuring_();
      }
      break;
    default:
      break;
  }
}

bool IM891AXL::config_matches_(const hci::Message &message) {
  hci::GatewayConfig got;
  if (!hci::GatewayConfig::decode(message.payload.subspan(1), got)) {
    ESP_LOGW(TAG, "The configuration of the module is too short: %u bytes", (unsigned) (message.payload.size() - 1));
    return false;
  }
  if (got.matches(this->wanted_))
    return true;
  ESP_LOGW(TAG,
           "Module configuration: link mode %u, options %04X, UI options %04X, LED %u ms, re-calibration %" PRIu32
           " ms. Wanted: %u, %04X, %04X, %u ms, %" PRIu32 " ms",
           got.link_mode, got.options, got.ui_options, got.led_flash_ms, got.recalibration_timeout_ms,
           this->wanted_.link_mode, this->wanted_.options, this->wanted_.ui_options, this->wanted_.led_flash_ms,
           this->wanted_.recalibration_timeout_ms);
  return false;
}

// ---------------------------------------------------------------------------------------------------------------
// ROM bootloader and RAM images
// ---------------------------------------------------------------------------------------------------------------

void IM891AXL::set_uart_parity_(uart::UARTParityOptions parity) {
  if (this->parent_->get_parity() == parity)
    return;
  // the last bytes leave in the old format
  this->flush();
  this->parent_->set_parity(parity);
  this->parent_->load_settings(false);
}

const RamImage *IM891AXL::find_image_(std::string_view name) const {
  for (const auto &image : this->images_) {
    if (image.name == name)
      return &image;
  }
  return nullptr;
}

bool IM891AXL::start_bootloader(bool via_hci) {
  if (this->state_ == State::STATE_BOOTLOADER_REQUEST || this->is_in_bootloader()) {
    ESP_LOGW(TAG, "The module is going to the bootloader, or is there");
    return false;
  }
  if (!via_hci) {
    ESP_LOGI(TAG, "Talking to the ROM bootloader, the module is in it already");
    this->enter_bootloader_wait_();
    return true;
  }
  // The module's firmware answers the request, and so does a RAM image that was started earlier
  if (!this->is_ready() && !this->is_image_running()) {
    ESP_LOGW(TAG, "The module does not answer HCI requests now, cannot restart it into the bootloader");
    return false;
  }
  // HCI is 8N1, which a RAM image may not be using
  this->set_uart_parity_(uart::UART_CONFIG_PARITY_NONE);
  this->decoder_.reset();
  this->send_request_(State::STATE_BOOTLOADER_REQUEST, hci::SAP_DEVICE_MANAGEMENT, hci::DEVMGMT_START_BOOTLOADER_REQ);
  return true;
}

bool IM891AXL::load_image(std::string_view name) {
  const RamImage *image = this->find_image_(name);
  if (image == nullptr) {
    ESP_LOGE(TAG, "There is no RAM image named \"%.*s\"", (int) name.size(), name.data());
    return false;
  }
  if (this->state_ == State::STATE_BOOTLOADER && this->bootloader_.ready()) {
    this->start_loading_(*image, App.get_loop_component_start_time());
    return true;
  }
  if (this->pending_image_ != nullptr || this->loading_image_ != nullptr) {
    ESP_LOGW(TAG, "An image is being loaded already");
    return false;
  }
  this->pending_image_ = image;
  // on its way to the bootloader, the image is loaded when that is ready
  if (this->state_ == State::STATE_BOOTLOADER_REQUEST || this->is_in_bootloader())
    return true;
  if (!this->start_bootloader(true)) {
    this->pending_image_ = nullptr;
    return false;
  }
  return true;
}

void IM891AXL::reset() {
  ESP_LOGI(TAG, "Resetting the module");
  this->pending_image_ = nullptr;
  this->loading_image_ = nullptr;
  this->set_uart_parity_(uart::UART_CONFIG_PARITY_NONE);
  this->start_reset_(0);
}

// From here the UART has the format of the bootloader, 8 data bits and even parity
void IM891AXL::enter_bootloader_wait_() {
  this->pending_ = false;
  this->set_uart_parity_(uart::UART_CONFIG_PARITY_EVEN);
  this->discard_available();
  this->state_ = State::STATE_BOOTLOADER_WAIT;
  this->state_since_ = App.get_loop_component_start_time();
}

void IM891AXL::start_loading_(const RamImage &image, uint32_t now) {
  this->pending_image_ = nullptr;
  this->loading_image_ = &image;
  this->last_progress_ = 0;
  ESP_LOGI(TAG, "Writing %u bytes of image %s to 0x%08" PRIX32 "%s", (unsigned) image.size, image.name, image.address,
           image.verify ? LOG_STR_LITERAL(", then reading them back") : LOG_STR_LITERAL(""));
  this->bootloader_.load({image.data, image.size, image.address, image.verify, true}, now);
}

void IM891AXL::loop_bootloader_(uint32_t now) {
  this->read_available_chunks(
      [this, now](const uint8_t *data, size_t len) { this->bootloader_.receive({data, len}, now); });
  this->bootloader_.poll(now);
  const auto tx = this->bootloader_.tx();
  if (!tx.empty()) {
    this->write_array(tx.data(), tx.size());
    this->bootloader_.clear_tx();
  }

  if (this->bootloader_.failed()) {
    this->leave_bootloader_failed_();
    return;
  }
  if (this->bootloader_.done()) {
    this->enter_image_mode_();
    return;
  }
  if (this->bootloader_.ready()) {
    if (!this->bootloader_ready_logged_) {
      this->bootloader_ready_logged_ = true;
      ESP_LOGD(TAG, "ROM bootloader version %u.%u, product ID %04X", this->bootloader_.version() >> 4,
               this->bootloader_.version() & 0x0F, this->bootloader_.device_id());
    }
    if (this->pending_image_ != nullptr)
      this->start_loading_(*this->pending_image_, now);
    return;
  }
  // progress in quarters of the image
  if (this->loading_image_ != nullptr) {
    const size_t size = this->loading_image_->size;
    const uint8_t pass = this->bootloader_.verifying() ? 1 : 0;
    const uint8_t progress = pass * 100 + uint8_t(this->bootloader_.offset() * 100 / size);
    if (progress / 25 != this->last_progress_ / 25) {
      this->last_progress_ = progress;
      ESP_LOGD(TAG, "%s %u%%", pass ? LOG_STR_LITERAL("Verifying") : LOG_STR_LITERAL("Writing"), progress % 100);
    }
  }
}

void IM891AXL::leave_bootloader_failed_() {
  ESP_LOGE(TAG, "ROM bootloader: %s in %s (0x%" PRIX32 ")",
           LOG_STR_ARG(BootloaderErrorStrings::get_log_str(this->bootloader_.error())),
           LOG_STR_ARG(BootloaderStateStrings::get_log_str(this->bootloader_.error_state())),
           this->bootloader_.error_value());
  this->pending_image_ = nullptr;
  this->loading_image_ = nullptr;
  this->set_uart_parity_(uart::UART_CONFIG_PARITY_NONE);
  if (this->reset_pin_ == nullptr) {
    ESP_LOGW(TAG, "There is no reset pin, the module stays in the bootloader until it is power cycled");
  }
  this->status_set_warning();
  // The reset starts the module's own firmware again
  this->start_reset_(RETRY_DELAY_MS);
}

// From here the UART has the format of the RAM image
void IM891AXL::enter_image_mode_() {
  const RamImage *image = this->loading_image_;
  this->loading_image_ = nullptr;
  ESP_LOGI(TAG, "Started image %s at 0x%08" PRIX32, image->name, image->address);
  this->set_uart_parity_(this->image_parity_);
  this->discard_available();
  this->decoder_.reset();
  this->image_line_.reset();
  this->status_clear_warning();
  this->state_ = State::STATE_IMAGE_RUNNING;
}

void IM891AXL::leave_image_mode_() {
  ESP_LOGI(TAG, "A valid HCI frame arrived: the module restarted into its own firmware");
  this->set_uart_parity_(uart::UART_CONFIG_PARITY_NONE);
  this->decoder_.reset();
  this->image_line_.reset();
  // the firmware needs a moment more before it answers
  this->state_ = State::STATE_BOOTING;
  this->state_since_ = App.get_loop_component_start_time() - (BOOT_TIME_MS - 300);
}

void IM891AXL::loop_image_() {
  this->read_available([this](uint8_t byte) { return this->on_image_byte_(byte); });
}

bool IM891AXL::on_image_byte_(uint8_t byte) {
  // Only a whole HCI frame with a good CRC is the module's firmware: that does not happen by chance in the output of a
  // program, whatever it prints
  if (this->image_detect_restart_ && this->decoder_.push(byte)) {
    this->leave_image_mode_();
    return false;
  }
  if (byte != '\n' && byte != '\r' && (byte < 0x20 || byte >= 0x7F))
    byte = '.';
  size_t len = 0;
  const char *line = this->image_line_.feed(byte, len);
  if (line != nullptr) {
    this->publish_image_line_(line, len);
  } else if (this->image_line_.overflowed()) {
    // a longer line is passed on in pieces
    this->publish_image_line_(this->image_line_.partial(), MAX_IMAGE_LINE);
    this->image_line_.restart_with(byte);
  }
  return true;
}

void IM891AXL::publish_image_line_(const char *line, size_t len) {
  if (len == 0)
    return;
  ESP_LOGV(TAG, "image: %s", line);
  this->image_line_callback_.call(StringRef(line, len));
}

}  // namespace esphome::im891a_xl

#pragma once

// Client of the ST ROM bootloader of the STM32WLE5 inside the iM891A-XL, the USART protocol of ST's AN3155.
// The module's firmware starts the bootloader on the undocumented HCI request StartBootloader (SAP 01, message F1),
// and the bootloader speaks 8 data bits with even parity, not the 8N1 of HCI.
//
// The client does no I/O: it is fed the bytes that arrive, says which bytes to send, and is told the time.
// Standard library only, so that it can be tested on the host against a simulated bootloader, and constexpr, so that
// the tests are static_asserts. Nothing here allocates. The texts for the states and errors are tables in the
// component.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace esphome::im891a_xl::bootloader {

constexpr uint8_t SYNC = 0x7F;
constexpr uint8_t ACK = 0x79;
constexpr uint8_t NACK = 0x1F;

constexpr uint8_t CMD_GET = 0x00;
constexpr uint8_t CMD_GET_ID = 0x02;
constexpr uint8_t CMD_READ_MEMORY = 0x11;
constexpr uint8_t CMD_GO = 0x21;
constexpr uint8_t CMD_WRITE_MEMORY = 0x31;

/// Product ID of the STM32WLE5 and STM32WL55
constexpr uint16_t DEVICE_ID_STM32WLE5 = 0x0497;

/// A write or read moves at most 256 bytes
constexpr size_t MAX_CHUNK = 256;
/// The most the client sends at once is a chunk to write with its length and checksum. The bootloader answers before
/// the next step, so a little more is slack: a client that is asked for more than this fails.
constexpr size_t MAX_TX = MAX_CHUNK + 2 + 8;

constexpr uint32_t SYNC_RETRY_MS = 200;
constexpr uint8_t SYNC_TRIES = 20;
constexpr uint32_t REPLY_TIMEOUT_MS = 1000;
// the bootloader writes a chunk into RAM, or reads one out, before it answers
constexpr uint32_t MEMORY_TIMEOUT_MS = 2000;

/// What to put into RAM, and what to do with it.
struct Image {
  const uint8_t *data{nullptr};
  size_t size{0};
  uint32_t address{0};  // where it is written, and where Go starts it: the vector table is at its start
  bool verify{true};    // read it back and compare
  bool go{true};        // start it
};

class Client {
 public:
  enum class State : uint8_t {
    STATE_IDLE,
    STATE_SYNC,
    STATE_GET_ACK,
    STATE_GET_LENGTH,
    STATE_GET_DATA,
    STATE_GET_END,
    STATE_ID_ACK,
    STATE_ID_LENGTH,
    STATE_ID_DATA,
    STATE_ID_END,
    STATE_READY,  // synchronised, the product ID is known: ready for load()
    STATE_WRITE_COMMAND,
    STATE_WRITE_ADDRESS,
    STATE_WRITE_DATA,
    STATE_READ_COMMAND,
    STATE_READ_ADDRESS,
    STATE_READ_LENGTH,
    STATE_READ_DATA,
    STATE_GO_COMMAND,
    STATE_GO_ADDRESS,
    STATE_DONE,  // everything asked for is done, and the image runs if it was started
    STATE_FAILED,
  };

  /// Why the client is in STATE_FAILED. error_state() and error_value() say more.
  enum class Error : uint8_t {
    ERROR_NONE,
    ERROR_NOT_READY,  // load() outside of STATE_READY
    ERROR_EMPTY_IMAGE,
    ERROR_MISSING_COMMAND,  // the bootloader lacks a command the image needs
    ERROR_TX_OVERFLOW,      // more to send than the buffer holds
    ERROR_NO_SYNC,          // no answer to the sync byte
    ERROR_TIMEOUT,          // no answer in error_state()
    ERROR_NACK,             // NACK in error_state()
    ERROR_UNEXPECTED_BYTE,  // error_value() instead of an ACK in error_state()
    ERROR_WRONG_PRODUCT,    // the product ID is error_value()
    ERROR_VERIFY,           // the RAM differs from the image at offset error_value()
  };

  /// Starts with the sync byte, then Get and Get ID. It ends in READY, or FAILED if the product is not the expected
  /// one.
  constexpr void begin(uint16_t expected_device_id, uint32_t now) {
    this->reset_();
    this->expected_device_id_ = expected_device_id;
    this->sync_tries_ = 0;
    this->stale_nacks_ = 0;
    this->state_ = State::STATE_SYNC;
    this->send_sync_(now);
  }

  /// Writes the image, and reads it back and starts it as the image says. Only in READY.
  constexpr bool load(const Image &image, uint32_t now) {
    if (this->state_ != State::STATE_READY) {
      this->fail_(Error::ERROR_NOT_READY);
      return false;
    }
    if (image.data == nullptr || image.size == 0) {
      this->fail_(Error::ERROR_EMPTY_IMAGE);
      return false;
    }
    if (!this->supports(CMD_WRITE_MEMORY) || (image.verify && !this->supports(CMD_READ_MEMORY)) ||
        (image.go && !this->supports(CMD_GO))) {
      this->fail_(Error::ERROR_MISSING_COMMAND);
      return false;
    }
    this->image_ = image;
    this->offset_ = 0;
    this->start_write_(now);
    return true;
  }

  /// Bytes that arrived from the module
  constexpr void receive(std::span<const uint8_t> data, uint32_t now) {
    for (size_t i = 0; i < data.size() && this->state_ != State::STATE_FAILED; i++) {
      this->last_activity_ = now;
      this->on_byte_(data[i], now);
    }
  }

  /// Call regularly: retries the sync byte, and gives up on a bootloader that stopped answering.
  constexpr void poll(uint32_t now) {
    if (this->state_ == State::STATE_SYNC) {
      if (now - this->last_sent_ >= SYNC_RETRY_MS) {
        if (this->sync_tries_ >= SYNC_TRIES) {
          this->fail_(Error::ERROR_NO_SYNC);
        } else {
          this->send_sync_(now);
        }
      }
    } else if (this->waiting_() && now - this->last_activity_ >= this->timeout_()) {
      this->fail_(Error::ERROR_TIMEOUT);
    }
  }

  /// The bytes to send. They stay until clear_tx().
  constexpr std::span<const uint8_t> tx() const { return {this->tx_.data(), this->tx_len_}; }
  constexpr void clear_tx() { this->tx_len_ = 0; }

  constexpr State state() const { return this->state_; }
  constexpr bool ready() const { return this->state_ == State::STATE_READY; }
  constexpr bool done() const { return this->state_ == State::STATE_DONE; }
  constexpr bool failed() const { return this->state_ == State::STATE_FAILED; }
  constexpr Error error() const { return this->error_; }
  /// The state the client was in when it failed
  constexpr State error_state() const { return this->error_state_; }
  /// The unexpected byte, the product ID, or the offset that differs, as error() says
  constexpr uint32_t error_value() const { return this->error_value_; }
  constexpr uint16_t device_id() const { return this->device_id_; }
  constexpr uint8_t version() const { return this->version_; }
  /// Whether the bootloader listed the command in its answer to Get
  constexpr bool supports(uint8_t command) const { return this->commands_[command / 32] >> (command % 32) & 1; }
  /// How many commands it listed
  constexpr size_t command_count() const { return this->command_count_; }
  /// Bytes of the image written so far, or compared while verifying
  constexpr size_t offset() const { return this->offset_; }
  constexpr bool verifying() const {
    return this->state_ == State::STATE_READ_COMMAND || this->state_ == State::STATE_READ_ADDRESS ||
           this->state_ == State::STATE_READ_LENGTH || this->state_ == State::STATE_READ_DATA;
  }

 protected:
  constexpr void reset_() {
    this->tx_len_ = 0;
    this->got_ = 0;
    this->commands_ = {};
    this->command_count_ = 0;
    this->error_ = Error::ERROR_NONE;
    this->error_state_ = State::STATE_IDLE;
    this->error_value_ = 0;
    this->image_ = Image();
    this->offset_ = 0;
    this->chunk_ = 0;
    this->need_ = 0;
    this->device_id_ = 0;
    this->version_ = 0;
    this->state_ = State::STATE_IDLE;
  }

  constexpr bool waiting_() const {
    return this->state_ != State::STATE_IDLE && this->state_ != State::STATE_READY &&
           this->state_ != State::STATE_DONE && this->state_ != State::STATE_FAILED;
  }

  constexpr uint32_t timeout_() const {
    switch (this->state_) {
      case State::STATE_WRITE_DATA:
      case State::STATE_READ_DATA:
        return MEMORY_TIMEOUT_MS;
      default:
        return REPLY_TIMEOUT_MS;
    }
  }

  constexpr void fail_(Error error, uint32_t value = 0) {
    this->error_ = error;
    this->error_state_ = this->state_;
    this->error_value_ = value;
    this->state_ = State::STATE_FAILED;
    this->tx_len_ = 0;
  }

  constexpr void send_(uint8_t byte, uint32_t now) {
    if (this->state_ == State::STATE_FAILED)
      return;
    if (this->tx_len_ == this->tx_.size()) {
      this->fail_(Error::ERROR_TX_OVERFLOW);
      return;
    }
    this->tx_[this->tx_len_++] = byte;
    this->last_sent_ = now;
    this->last_activity_ = now;
  }

  // The next state after sending, unless the sending failed
  constexpr void go_(State next) {
    if (this->state_ != State::STATE_FAILED)
      this->state_ = next;
  }

  constexpr void send_sync_(uint32_t now) {
    this->sync_tries_++;
    this->send_(SYNC, now);
  }

  // A command is the byte and its complement
  constexpr void send_command_(uint8_t command, State next, uint32_t now) {
    this->send_(command, now);
    this->send_(command ^ 0xFF, now);
    this->go_(next);
  }

  // An address is 4 bytes, most significant first, and their XOR
  constexpr void send_address_(uint32_t address, State next, uint32_t now) {
    const uint8_t a[4] = {uint8_t(address >> 24), uint8_t(address >> 16), uint8_t(address >> 8), uint8_t(address)};
    for (const uint8_t b : a)
      this->send_(b, now);
    this->send_(a[0] ^ a[1] ^ a[2] ^ a[3], now);
    this->go_(next);
  }

  // The ACK that every step but the data of a read starts with
  constexpr bool ack_(uint8_t byte) {
    if (byte == ACK)
      return true;
    if (byte == NACK) {
      this->fail_(Error::ERROR_NACK);
    } else {
      this->fail_(Error::ERROR_UNEXPECTED_BYTE, byte);
    }
    return false;
  }

  constexpr size_t chunk_size_() const {
    const size_t left = this->image_.size - this->offset_;
    return left < MAX_CHUNK ? left : MAX_CHUNK;
  }

  constexpr void start_write_(uint32_t now) {
    this->chunk_ = this->chunk_size_();
    this->send_command_(CMD_WRITE_MEMORY, State::STATE_WRITE_COMMAND, now);
  }

  constexpr void start_read_(uint32_t now) {
    this->chunk_ = this->chunk_size_();
    this->send_command_(CMD_READ_MEMORY, State::STATE_READ_COMMAND, now);
  }

  // What follows when a whole pass over the image is done
  constexpr void next_phase_(bool after_write, uint32_t now) {
    if (after_write && this->image_.verify) {
      this->offset_ = 0;
      this->start_read_(now);
    } else if (this->image_.go) {
      this->send_command_(CMD_GO, State::STATE_GO_COMMAND, now);
    } else {
      this->state_ = State::STATE_DONE;
    }
  }

  constexpr void on_byte_(uint8_t b, uint32_t now) {
    switch (this->state_) {
      case State::STATE_SYNC:
        // Anything else is noise from the reset, or from the HCI firmware that was there before
        if (b == ACK || b == NACK) {
          // Each sync byte sent after the one that was answered gets a NACK of its own: it is read as a command
          this->stale_nacks_ = this->sync_tries_ - 1;
          this->send_command_(CMD_GET, State::STATE_GET_ACK, now);
        }
        break;
      case State::STATE_GET_ACK:
        if (b == NACK && this->stale_nacks_ > 0) {
          this->stale_nacks_--;
        } else if (this->ack_(b)) {
          this->state_ = State::STATE_GET_LENGTH;
        }
        break;
      case State::STATE_GET_LENGTH:
        this->need_ = size_t(b) + 1;  // the version, and N commands
        this->got_ = 0;
        this->state_ = State::STATE_GET_DATA;
        break;
      case State::STATE_GET_DATA:
        if (this->got_++ == 0) {
          this->version_ = b;
        } else {
          this->commands_[b / 32] |= uint32_t(1) << (b % 32);
          this->command_count_++;
        }
        if (this->got_ == this->need_)
          this->state_ = State::STATE_GET_END;
        break;
      case State::STATE_GET_END:
        if (this->ack_(b))
          this->send_command_(CMD_GET_ID, State::STATE_ID_ACK, now);
        break;
      case State::STATE_ID_ACK:
        if (this->ack_(b))
          this->state_ = State::STATE_ID_LENGTH;
        break;
      case State::STATE_ID_LENGTH:
        this->need_ = size_t(b) + 1;
        this->got_ = 0;
        this->device_id_ = 0;
        this->state_ = State::STATE_ID_DATA;
        break;
      case State::STATE_ID_DATA:
        // the last two bytes are the product ID
        this->device_id_ = uint16_t(this->device_id_ << 8 | b);
        if (++this->got_ == this->need_)
          this->state_ = State::STATE_ID_END;
        break;
      case State::STATE_ID_END:
        if (!this->ack_(b))
          break;
        if (this->device_id_ != this->expected_device_id_) {
          this->fail_(Error::ERROR_WRONG_PRODUCT, this->device_id_);
        } else {
          this->state_ = State::STATE_READY;
        }
        break;
      case State::STATE_WRITE_COMMAND:
        if (this->ack_(b))
          this->send_address_(this->image_.address + this->offset_, State::STATE_WRITE_ADDRESS, now);
        break;
      case State::STATE_WRITE_ADDRESS:
        if (!this->ack_(b))
          break;
        {
          // N-1, the data padded with 0xFF to a multiple of 4, and the XOR of all of that
          const size_t padded = (this->chunk_ + 3) & ~size_t(3);
          this->send_(uint8_t(padded - 1), now);
          uint8_t x = uint8_t(padded - 1);
          for (size_t i = 0; i < padded; i++) {
            const uint8_t d = i < this->chunk_ ? this->image_.data[this->offset_ + i] : 0xFF;
            this->send_(d, now);
            x ^= d;
          }
          this->send_(x, now);
          this->go_(State::STATE_WRITE_DATA);
        }
        break;
      case State::STATE_WRITE_DATA:
        if (!this->ack_(b))
          break;
        this->offset_ += this->chunk_;
        if (this->offset_ < this->image_.size) {
          this->start_write_(now);
        } else {
          this->next_phase_(true, now);
        }
        break;
      case State::STATE_READ_COMMAND:
        if (this->ack_(b))
          this->send_address_(this->image_.address + this->offset_, State::STATE_READ_ADDRESS, now);
        break;
      case State::STATE_READ_ADDRESS:
        if (this->ack_(b)) {
          this->send_(uint8_t(this->chunk_ - 1), now);
          this->send_(uint8_t(this->chunk_ - 1) ^ 0xFF, now);
          this->go_(State::STATE_READ_LENGTH);
        }
        break;
      case State::STATE_READ_LENGTH:
        if (this->ack_(b)) {
          this->got_ = 0;
          this->state_ = State::STATE_READ_DATA;
        }
        break;
      case State::STATE_READ_DATA:
        if (b != this->image_.data[this->offset_ + this->got_]) {
          this->fail_(Error::ERROR_VERIFY, this->offset_ + this->got_);
          break;
        }
        if (++this->got_ == this->chunk_) {
          this->offset_ += this->chunk_;
          if (this->offset_ < this->image_.size) {
            this->start_read_(now);
          } else {
            this->next_phase_(false, now);
          }
        }
        break;
      case State::STATE_GO_COMMAND:
        if (this->ack_(b))
          this->send_address_(this->image_.address, State::STATE_GO_ADDRESS, now);
        break;
      case State::STATE_GO_ADDRESS:
        // The bootloader answers, and then jumps to the image
        if (this->ack_(b))
          this->state_ = State::STATE_DONE;
        break;
      default:
        // Nothing is expected: late bytes of an earlier step
        break;
    }
  }

  Image image_;
  std::array<uint8_t, MAX_TX> tx_{};
  // one bit for each command the bootloader supports
  std::array<uint32_t, 8> commands_{};
  size_t tx_len_{0};
  size_t command_count_{0};
  size_t offset_{0};
  size_t chunk_{0};
  size_t need_{0};  // bytes of the answer of Get or Get ID
  size_t got_{0};   // bytes of that answer, or of the chunk being read back, that arrived
  uint32_t error_value_{0};
  uint32_t last_sent_{0};
  uint32_t last_activity_{0};
  uint16_t expected_device_id_{DEVICE_ID_STM32WLE5};
  uint16_t device_id_{0};
  State state_{State::STATE_IDLE};
  State error_state_{State::STATE_IDLE};
  Error error_{Error::ERROR_NONE};
  uint8_t version_{0};
  uint8_t sync_tries_{0};
  uint8_t stale_nacks_{0};
};

}  // namespace esphome::im891a_xl::bootloader

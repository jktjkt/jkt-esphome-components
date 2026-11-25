// Compile-time tests of bootloader.h against a simulated ST ROM bootloader (AN3155). If this file compiles, the tests
// passed:
//   g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/im891a_xl -fsyntax-only bootloader_test.cpp
//
// The simulation is written separately from the client: it parses the bytes it gets, keeps a RAM, and can be made to
// misbehave.
#include "bootloader.h"

#include <algorithm>
#include <vector>

using namespace esphome::im891a_xl::bootloader;
using Error = Client::Error;
using State = Client::State;

namespace {

// A failing check calls this, which is not constexpr, so the compiler names the check in its error
void check_failed(const char *) {}
#define CHECK(cond) \
  do { \
    if (!(cond)) \
      check_failed(#cond); \
  } while (0)

// The simulation allocates, which constexpr allows as long as it is freed before the end of the evaluation.
// bootloader.h does not.
using Bytes = std::vector<uint8_t>;

constexpr uint32_t RAM_BASE = 0x20008000;
constexpr size_t RAM_SIZE = 0x8000;

struct FakeBootloader {
  // behaviour
  uint16_t device_id = 0x0497;
  Bytes commands = {CMD_GET, CMD_GET_ID, CMD_READ_MEMORY, CMD_GO, CMD_WRITE_MEMORY};
  int ignore_syncs = 0;    // the bootloader is not up yet
  bool nack_read = false;  // read-out protection
  int corrupt_at = -1;     // a RAM byte that changes after the write
  bool mute = false;       // stops answering
  // what it did
  Bytes ram = Bytes(RAM_SIZE, 0x00);
  bool went = false;
  uint32_t go_address = 0;
  int sync_bytes_seen = 0;
  Bytes out;

  constexpr void feed(uint8_t b) {
    if (mute)
      return;
    if (!synced) {
      if (b != SYNC)
        return;
      sync_bytes_seen++;
      if (ignore_syncs > 0) {
        ignore_syncs--;
        return;
      }
      synced = true;
      out.push_back(ACK);
      return;
    }
    switch (st) {
      case St::COMMAND:
        if (b == SYNC) {  // a sync byte once synchronised is no command
          sync_bytes_seen++;
          out.push_back(NACK);
          return;
        }
        in.push_back(b);
        if (in.size() == 2) {
          const uint8_t c = in[0];
          const bool valid = in[1] == (c ^ 0xFF);
          in.clear();
          command(c, valid);
        }
        break;
      case St::WRITE_ADDRESS:
        in.push_back(b);
        if (in.size() == 5) {
          if (!address_ok()) {
            st = St::COMMAND;
            out.push_back(NACK);
          } else {
            out.push_back(ACK);
            st = St::WRITE_DATA;
          }
        }
        break;
      case St::WRITE_DATA: {
        in.push_back(b);
        // N-1, N bytes of data, the XOR
        if (in.size() >= 2 && in.size() == size_t(in[0]) + 1 + 1 + 1) {
          uint8_t x = 0;
          for (size_t i = 0; i + 1 < in.size(); i++)
            x ^= in[i];
          const size_t n = size_t(in[0]) + 1;
          const uint32_t at = address - RAM_BASE;
          if (x != in.back() || n % 4 != 0 || at + n > RAM_SIZE) {
            out.push_back(NACK);
          } else {
            for (size_t i = 0; i < n; i++)
              ram[at + i] = in[1 + i];
            if (corrupt_at >= 0 && size_t(corrupt_at) < RAM_SIZE)
              ram[corrupt_at] ^= 0x01;
            out.push_back(ACK);
          }
          in.clear();
          st = St::COMMAND;
        }
        break;
      }
      case St::READ_ADDRESS:
        in.push_back(b);
        if (in.size() == 5) {
          if (!address_ok()) {
            st = St::COMMAND;
            out.push_back(NACK);
          } else {
            out.push_back(ACK);
            st = St::READ_LENGTH;
          }
        }
        break;
      case St::READ_LENGTH:
        in.push_back(b);
        if (in.size() == 2) {
          const bool valid = in[1] == (in[0] ^ 0xFF);
          const size_t n = size_t(in[0]) + 1;
          in.clear();
          st = St::COMMAND;
          if (!valid) {
            out.push_back(NACK);
          } else {
            out.push_back(ACK);
            for (size_t i = 0; i < n; i++)
              out.push_back(ram[address - RAM_BASE + i]);
          }
        }
        break;
      case St::GO_ADDRESS:
        in.push_back(b);
        if (in.size() == 5) {
          st = St::COMMAND;
          if (!address_ok()) {
            out.push_back(NACK);
          } else {
            out.push_back(ACK);
            went = true;
            go_address = address;
          }
        }
        break;
    }
  }

 private:
  enum class St { COMMAND, WRITE_ADDRESS, WRITE_DATA, READ_ADDRESS, READ_LENGTH, GO_ADDRESS };
  St st = St::COMMAND;
  bool synced = false;
  Bytes in;
  uint32_t address = 0;

  constexpr bool address_ok() {
    uint8_t x = 0;
    for (int i = 0; i < 4; i++)
      x ^= in[i];
    const bool ok = x == in[4];
    address = uint32_t(in[0]) << 24 | uint32_t(in[1]) << 16 | uint32_t(in[2]) << 8 | in[3];
    in.clear();
    return ok && address >= RAM_BASE && address < RAM_BASE + RAM_SIZE;
  }

  constexpr void command(uint8_t c, bool valid) {
    if (!valid) {
      out.push_back(NACK);
      return;
    }
    switch (c) {
      case CMD_GET:
        out.push_back(ACK);
        out.push_back(uint8_t(commands.size()));  // N: the version and N commands follow, N+1 bytes
        out.push_back(0x31);                      // bootloader version 3.1
        out.insert(out.end(), commands.begin(), commands.end());
        out.push_back(ACK);
        break;
      case CMD_GET_ID:
        out.push_back(ACK);
        out.push_back(0x01);
        out.push_back(device_id >> 8);
        out.push_back(device_id & 0xFF);
        out.push_back(ACK);
        break;
      case CMD_WRITE_MEMORY:
        out.push_back(ACK);
        st = St::WRITE_ADDRESS;
        break;
      case CMD_READ_MEMORY:
        if (nack_read) {
          out.push_back(NACK);
        } else {
          out.push_back(ACK);
          st = St::READ_ADDRESS;
        }
        break;
      case CMD_GO:
        out.push_back(ACK);
        st = St::GO_ADDRESS;
        break;
      default:
        out.push_back(NACK);
    }
  }
};

// The client and the simulated bootloader, with a clock. A reply is delivered a step after the request.
struct Sim {
  Client client;
  FakeBootloader fake;
  uint32_t now = 1000;
  bool hold_replies = false;  // delivered late, to see what the client does with a slow bootloader

  constexpr void step(uint32_t ms = 1) {
    now += ms;
    client.poll(now);
    for (const uint8_t b : client.tx())
      fake.feed(b);
    client.clear_tx();
    if (!hold_replies && !fake.out.empty()) {
      client.receive(fake.out, now);
      fake.out.clear();
    }
  }

  template<typename Until> constexpr bool run(Until until, uint32_t max_ms = 30000) {
    for (uint32_t t = 0; t < max_ms; t++) {
      if (until())
        return true;
      step();
    }
    return until();
  }

  constexpr bool settled() const { return client.ready() || client.done() || client.failed(); }
  constexpr bool finished() const { return client.done() || client.failed(); }
};

constexpr Bytes make_image(size_t n) {
  Bytes d(n);
  uint32_t x = 12345;
  for (auto &b : d) {
    x = x * 1664525 + 1013904223;
    b = x >> 24;
  }
  return d;
}

// Synchronised, Get and Get ID done
constexpr bool to_ready(Sim &s) {
  s.client.begin(DEVICE_ID_STM32WLE5, s.now);
  return s.run([&] { return s.settled(); }) && s.client.ready();
}

// Writes the image at the address, as far as it goes
constexpr bool load(Sim &s, const Bytes &image, uint32_t address, bool verify = true, bool go = true) {
  if (!s.client.load({image.data(), image.size(), address, verify, go}, s.now))
    return false;
  return s.run([&] { return s.finished(); });
}

// the whole sequence: sync, Get, Get ID, write, read back, go. 701 bytes: three chunks, the last one padded
constexpr bool whole_sequence() {
  Sim s;
  const Bytes image = make_image(701);
  CHECK(to_ready(s));
  CHECK(s.client.device_id() == 0x0497);
  CHECK(s.client.version() == 0x31);
  CHECK(s.client.command_count() == s.fake.commands.size());
  for (const uint8_t c : s.fake.commands)
    CHECK(s.client.supports(c));
  CHECK(!s.client.supports(0x43) && !s.client.supports(0xFF));
  CHECK(load(s, image, RAM_BASE));
  CHECK(s.client.done());
  CHECK(s.client.error() == Error::ERROR_NONE);
  CHECK(std::equal(image.begin(), image.end(), s.fake.ram.begin()));
  CHECK(s.fake.ram[701] == 0xFF && s.fake.ram[702] == 0xFF && s.fake.ram[703] == 0xFF);  // padding
  CHECK(s.fake.ram[704] == 0x00);
  CHECK(s.fake.went && s.fake.go_address == RAM_BASE);
  CHECK(s.fake.sync_bytes_seen == 1);
  return true;
}
static_assert(whole_sequence());

// an image of exactly one chunk, one of a single byte, and some more around the chunk size
constexpr bool sizes() {
  for (const size_t n : {size_t(256), size_t(1), size_t(512), size_t(257)}) {
    Sim s;
    const Bytes image = make_image(n);
    CHECK(to_ready(s));
    CHECK(load(s, image, RAM_BASE + 0x100));
    CHECK(s.client.done());
    CHECK(std::equal(image.begin(), image.end(), s.fake.ram.begin() + 0x100));
    CHECK(s.fake.go_address == RAM_BASE + 0x100);
  }
  return true;
}
static_assert(sizes());

// without verifying, and without going
constexpr bool write_only() {
  Sim s;
  const Bytes image = make_image(300);
  CHECK(to_ready(s));
  CHECK(load(s, image, RAM_BASE, false, false));
  CHECK(s.client.done() && !s.fake.went);
  CHECK(std::equal(image.begin(), image.end(), s.fake.ram.begin()));
  return true;
}
static_assert(write_only());

// a bootloader that is not up yet: the sync byte is sent again until it answers
constexpr bool late_bootloader() {
  Sim s;
  s.fake.ignore_syncs = 3;
  CHECK(to_ready(s));
  CHECK(s.fake.sync_bytes_seen == 4);
  return true;
}
static_assert(late_bootloader());

// a bootloader that never answers
constexpr bool no_bootloader() {
  Sim s;
  s.fake.ignore_syncs = 1000;
  s.client.begin(DEVICE_ID_STM32WLE5, s.now);
  CHECK(s.run([&] { return s.settled(); }));
  CHECK(s.client.failed() && s.client.error() == Error::ERROR_NO_SYNC);
  CHECK(s.fake.sync_bytes_seen == SYNC_TRIES);
  return true;
}
static_assert(no_bootloader());

// a slow bootloader: its ACK comes after the second sync byte, which it answers with a NACK, and that is no error
constexpr bool slow_bootloader() {
  Sim s;
  s.hold_replies = true;
  s.client.begin(DEVICE_ID_STM32WLE5, s.now);
  for (uint32_t t = 0; t < SYNC_RETRY_MS + 5; t++)
    s.step();
  CHECK(s.fake.sync_bytes_seen == 2 && s.fake.out == (Bytes{ACK, NACK}));
  s.hold_replies = false;
  CHECK(s.run([&] { return s.settled(); }));
  CHECK(s.client.ready());
  return true;
}
static_assert(slow_bootloader());

// another product
constexpr bool other_product() {
  Sim s;
  s.fake.device_id = 0x0450;
  CHECK(!to_ready(s));
  CHECK(s.client.failed() && s.client.error() == Error::ERROR_WRONG_PRODUCT);
  CHECK(s.client.error_value() == 0x0450);
  return true;
}
static_assert(other_product());

// read-out protection: the write works and the read-back is refused
constexpr bool read_protected() {
  Sim s;
  s.fake.nack_read = true;
  CHECK(to_ready(s));
  CHECK(load(s, make_image(100), RAM_BASE));
  CHECK(s.client.failed() && s.client.error() == Error::ERROR_NACK);
  CHECK(s.client.error_state() == State::STATE_READ_COMMAND);
  CHECK(!s.fake.went);
  return true;
}
static_assert(read_protected());

// the RAM does not hold what was written
constexpr bool corrupt_ram() {
  Sim s;
  s.fake.corrupt_at = 0x2A;
  CHECK(to_ready(s));
  CHECK(load(s, make_image(100), RAM_BASE));
  CHECK(s.client.failed() && s.client.error() == Error::ERROR_VERIFY);
  CHECK(s.client.error_value() == 0x2A);
  CHECK(!s.fake.went);
  return true;
}
static_assert(corrupt_ram());

// an address the bootloader refuses
constexpr bool bad_address() {
  Sim s;
  CHECK(to_ready(s));
  CHECK(load(s, make_image(100), 0x20000000));
  CHECK(s.client.failed() && s.client.error() == Error::ERROR_NACK);
  CHECK(s.client.error_state() == State::STATE_WRITE_ADDRESS);
  return true;
}
static_assert(bad_address());

// the bootloader stops answering in the middle of the write
constexpr bool stops_answering() {
  Sim s;
  const Bytes image = make_image(600);
  CHECK(to_ready(s));
  CHECK(s.client.load({image.data(), image.size(), RAM_BASE, true, true}, s.now));
  CHECK(s.run([&] { return s.client.offset() >= 256; }));
  s.fake.mute = true;
  CHECK(s.run([&] { return s.client.failed(); }, 10000));
  CHECK(s.client.error() == Error::ERROR_TIMEOUT);
  CHECK(s.client.error_state() == State::STATE_WRITE_DATA || s.client.error_state() == State::STATE_WRITE_ADDRESS ||
        s.client.error_state() == State::STATE_WRITE_COMMAND);
  return true;
}
static_assert(stops_answering());

// a bootloader without the commands that are needed
constexpr bool missing_commands() {
  Sim s;
  s.fake.commands = {CMD_GET, CMD_GET_ID, CMD_WRITE_MEMORY};
  const Bytes image = make_image(10);
  CHECK(to_ready(s));
  CHECK(s.client.command_count() == 3 && !s.client.supports(CMD_READ_MEMORY));
  CHECK(!s.client.load({image.data(), image.size(), RAM_BASE, true, true}, s.now));
  CHECK(s.client.failed() && s.client.error() == Error::ERROR_MISSING_COMMAND);
  return true;
}
static_assert(missing_commands());

// load() only in READY
constexpr bool load_not_ready() {
  Client c;
  const Bytes image = make_image(10);
  CHECK(!c.load({image.data(), image.size(), RAM_BASE, true, true}, 0));
  CHECK(c.failed() && c.error() == Error::ERROR_NOT_READY);
  return true;
}
static_assert(load_not_ready());

// bytes that nobody asked for are ignored while waiting for the sync
constexpr bool noise_before_sync() {
  Client c;
  c.begin(DEVICE_ID_STM32WLE5, 0);
  const std::array<uint8_t, 7> noise{0xC0, 0x01, 0xF2, 0x00, 0xA8, 0xD3, 0xC0};
  c.receive(noise, 10);
  CHECK(c.state() == State::STATE_SYNC);
  return true;
}
static_assert(noise_before_sync());

// a byte that is neither ACK nor NACK where an ACK is due
constexpr bool unexpected_byte() {
  Client c;
  c.begin(DEVICE_ID_STM32WLE5, 0);
  c.receive(std::array<uint8_t, 1>{ACK}, 1);
  CHECK(c.state() == State::STATE_GET_ACK);
  c.receive(std::array<uint8_t, 1>{0x42}, 2);
  CHECK(c.failed() && c.error() == Error::ERROR_UNEXPECTED_BYTE && c.error_value() == 0x42);
  CHECK(c.error_state() == State::STATE_GET_ACK && c.tx().empty());
  return true;
}
static_assert(unexpected_byte());

// What is to be sent waits in a buffer of a fixed size. ACKs that arrive before what they answer was taken out of it
// fill it, and the client fails instead of losing bytes, and stays failed.
constexpr bool tx_overflow() {
  Sim s;
  const Bytes image = make_image(600);
  CHECK(to_ready(s));
  CHECK(s.client.load({image.data(), image.size(), RAM_BASE, true, true}, s.now));
  s.client.clear_tx();
  // Write Memory command, address, the data of the first chunk, and again
  for (int i = 0; i < 6 && !s.client.failed(); i++)
    s.client.receive(std::array<uint8_t, 1>{ACK}, s.now);
  CHECK(s.client.failed() && s.client.error() == Error::ERROR_TX_OVERFLOW);
  CHECK(s.client.tx().empty());
  return true;
}
static_assert(tx_overflow());

// Everything is inline, the buffers are most of it
static_assert(sizeof(Client) <= 512);

}  // namespace

int main() { return 0; }

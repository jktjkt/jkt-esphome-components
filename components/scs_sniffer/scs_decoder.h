#pragma once

// SCS bus (BTicino / Legrand) decoder for edge-timed captures. Standard C++20 only and constexpr, so that
// tests/components/scs_sniffer/decoder_test.cpp checks it with static_assert on the host.
//
// On the RX pin of a passive tap, idle and '1' bits are high. A '0' bit is an active (low) pulse of about 34 us at
// the start of its 104.17 us bit cell (9600 baud). Bytes are framed like a UART: start bit, 8 data bits LSB first,
// stop bit. Only the falling edge of a pulse carries timing; its width only rejects glitches and a stuck-low line.
// Lows closer than merge_gap_ns are one pulse (ringing, chatter). Bits are placed by rounding the offset from the
// start bit to the nearest bit cell, so a sender clock error of a few percent is tolerated.
//
// A frame is A8 <payload> <checksum> A3, the checksum being the XOR of the payload. The payload is 4 bytes, or 8
// when the high nibble of its first byte is D or E. Payload and checksum may hold any value, A8 and A3 included,
// so a frame ends by its length. After a bad frame, decoding resumes at the next A8 inside it. A lone A5 outside a
// frame is an acknowledgement and arrives as a stray byte.
//
// The sink gets every decoded byte (on_byte), each byte outside a frame (on_stray), each valid frame (on_frame)
// and each error (on_error).

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::scs_sniffer {

static constexpr uint8_t FRAME_START = 0xA8;
static constexpr uint8_t FRAME_END = 0xA3;
static constexpr std::size_t SHORT_FRAME_SIZE = 7;
static constexpr std::size_t LONG_FRAME_SIZE = 11;
static constexpr std::size_t MAX_FRAME_SIZE = LONG_FRAME_SIZE;

struct Timing {
  uint32_t bit_ns = 104'167;          // 9600 baud
  uint32_t min_low_ns = 8'000;        // shorter active pulses are glitches and are ignored
  uint32_t max_low_ns = 80'000;       // nominal 34 us; longer is not an SCS bit
  uint32_t phase_tol_ns = 36'000;     // max distance of an edge from the start of its bit cell
  uint32_t frame_gap_ns = 1'500'000;  // idle after a byte that abandons an unterminated frame
  uint32_t merge_gap_ns = 25'000;     // shorter highs inside a pulse are bounces; real gaps are >= 56 us
};

enum class Error : uint8_t {
  ERROR_LONG_LOW,   // active pulse longer than max_low_ns (bus fault, wrong polarity, stretched signal)
  ERROR_BIT_PHASE,  // edge too far from any bit cell; byte dropped, edge taken as a new start bit
  ERROR_STOP_BIT,   // active pulse in the stop bit
  ERROR_END_BYTE,   // no A3 where the frame length puts it
  ERROR_CHECKSUM,   // XOR of the payload does not match
  ERROR_TRUNCATED,  // frame cut short by idle or the end of the capture
};

struct Frame {
  std::array<uint8_t, MAX_FRAME_SIZE> data{};
  uint8_t size = 0;

  constexpr uint8_t operator[](std::size_t i) const { return this->data[i]; }
  constexpr const uint8_t *begin() const { return this->data.data(); }
  constexpr const uint8_t *end() const { return this->data.data() + this->size; }
};

/// Total frame size, A8 and A3 included, given the first payload byte.
constexpr std::size_t frame_size(uint8_t first_payload_byte) {
  const uint8_t high_nibble = first_payload_byte >> 4;
  return high_nibble == 0xD || high_nibble == 0xE ? LONG_FRAME_SIZE : SHORT_FRAME_SIZE;
}

constexpr uint8_t checksum(const uint8_t *payload, std::size_t length) {
  uint8_t x = 0;
  for (std::size_t i = 0; i < length; ++i)
    x ^= payload[i];
  return x;
}

template<class Sink> class Decoder {
 public:
  constexpr explicit Decoder(Sink sink = Sink{}, Timing timing = Timing{}) : sink_(sink), timing_(timing) {}

  constexpr Sink &sink() { return this->sink_; }
  constexpr const Sink &sink() const { return this->sink_; }
  constexpr uint32_t glitches() const { return this->glitches_; }

  /// One stretch of constant level; `active` is the pulse level (low on the tap output). Consecutive segments of
  /// the same level are joined, so a capture may be split anywhere.
  constexpr void segment(bool active, uint32_t ns) {
    if (ns == 0)
      return;
    if (active) {
      if (!this->in_low_) {
        this->in_low_ = true;
        if (this->pending_ && this->now_ - this->pending_end_ < this->timing_.merge_gap_ns) {
          this->low_start_ = this->pending_start_;  // a bounce: continue the previous pulse
          this->pending_ = false;
        } else {
          this->flush_pending_();
          this->low_start_ = this->now_;
        }
      }
      this->now_ += ns;
      return;
    }
    if (this->in_low_) {
      this->in_low_ = false;
      this->pending_ = true;
      this->pending_start_ = this->low_start_;
      this->pending_end_ = this->now_;
    }
    this->now_ += ns;
    if (this->pending_ && this->now_ - this->pending_end_ >= this->timing_.merge_gap_ns)
      this->flush_pending_();
    if (!this->pending_)  // a pending pulse is older than now_ and must be seen first
      this->advance_(this->now_);
  }

  /// The capture ended after a long idle (the receiver's `idle:`). Time does not continue into the next capture,
  /// so everything pending is resolved now.
  constexpr void end_of_capture() {
    if (this->in_low_) {
      this->in_low_ = false;
      this->pending_ = true;
      this->pending_start_ = this->low_start_;
      this->pending_end_ = this->now_;
    }
    this->flush_pending_();
    if (this->in_byte_)
      this->finish_byte_();
    if (this->frame_.size != 0) {
      this->frame_.size = 0;
      this->sink_.on_error(Error::ERROR_TRUNCATED);
    }
    this->now_ = 0;
  }

  /// ESP-IDF rmt_symbol_word_t, or anything with duration0/level0/duration1/level1. A zero duration is the RMT
  /// end-of-capture marker. Pass last=false when feeding a capture in parts.
  template<class Symbol>
  constexpr void feed_rmt(const Symbol *symbols, std::size_t count, uint32_t tick_ns, bool last = true,
                          uint32_t active_level = 0) {
    for (std::size_t i = 0; i < count; ++i) {
      if (!this->rmt_half_(symbols[i].duration0, symbols[i].level0, tick_ns, active_level))
        return;
      if (!this->rmt_half_(symbols[i].duration1, symbols[i].level1, tick_ns, active_level))
        return;
    }
    if (last)
      this->end_of_capture();
  }

  /// Signed durations in microseconds, positive for an active pulse (mark) and negative for idle (space): the
  /// raw data of ESPHome's remote_receiver.
  template<class Int> constexpr void feed_marks_spaces(const Int *durations_us, std::size_t count, bool last = true) {
    for (std::size_t i = 0; i < count; ++i) {
      const auto d = durations_us[i];
      if (d > 0) {
        this->segment(true, static_cast<uint32_t>(d) * 1000u);
      } else if (d < 0) {
        this->segment(false, static_cast<uint32_t>(-d) * 1000u);
      }
    }
    if (last)
      this->end_of_capture();
  }

 protected:
  constexpr void flush_pending_() {
    if (!this->pending_)
      return;
    this->pending_ = false;
    this->pulse_(this->pending_start_, this->pending_end_ - this->pending_start_);
  }

  constexpr bool rmt_half_(uint32_t duration, uint32_t level, uint32_t tick_ns, uint32_t active_level) {
    if (duration == 0) {
      this->end_of_capture();
      return false;
    }
    this->segment(level == active_level, duration * tick_ns);
    return true;
  }

  /// Resolves everything that is decided by time `t` having passed without a new edge.
  constexpr void advance_(uint64_t t) {
    if (this->in_byte_ && t >= this->byte_start_ + uint64_t{this->timing_.bit_ns} * 19 / 2)  // mid stop bit
      this->finish_byte_();
    if (this->frame_.size != 0 && !this->in_byte_ && t >= this->byte_end_ + this->timing_.frame_gap_ns) {
      this->frame_.size = 0;
      this->sink_.on_error(Error::ERROR_TRUNCATED);
    }
  }

  constexpr void start_byte_(uint64_t start) {
    this->in_byte_ = true;
    this->byte_start_ = start;
    this->value_ = 0xFF;
  }

  constexpr void pulse_(uint64_t start, uint64_t width) {
    this->advance_(start);
    if (width < this->timing_.min_low_ns) {
      ++this->glitches_;
      return;
    }
    if (width > this->timing_.max_low_ns) {
      this->in_byte_ = false;
      this->frame_.size = 0;
      this->sink_.on_error(Error::ERROR_LONG_LOW);
      return;
    }
    if (!this->in_byte_) {
      this->start_byte_(start);
      return;
    }
    const uint64_t bit = this->timing_.bit_ns;
    const uint64_t rel = start - this->byte_start_;
    const uint64_t idx = (rel + bit / 2) / bit;
    const uint64_t cell = idx * bit;
    const uint64_t phase = rel > cell ? rel - cell : cell - rel;
    if (phase > this->timing_.phase_tol_ns) {
      this->frame_.size = 0;
      this->sink_.on_error(Error::ERROR_BIT_PHASE);
      this->start_byte_(start);
      return;
    }
    if (idx == 0) {  // second edge inside the start bit
      ++this->glitches_;
      return;
    }
    if (idx <= 8) {
      this->value_ = static_cast<uint8_t>(this->value_ & ~(1u << (idx - 1)));
      return;
    }
    // idx == 9; later edges are impossible because advance_() closes the byte mid stop bit
    this->in_byte_ = false;
    this->frame_.size = 0;
    this->sink_.on_error(Error::ERROR_STOP_BIT);
  }

  constexpr void finish_byte_() {
    this->in_byte_ = false;
    this->byte_end_ = this->byte_start_ + uint64_t{this->timing_.bit_ns} * 10;
    this->sink_.on_byte(this->value_);
    this->frame_byte_(this->value_);
  }

  constexpr void frame_byte_(uint8_t b) {
    if (this->frame_.size == 0 && b != FRAME_START) {
      this->sink_.on_stray(b);
      return;
    }
    this->frame_.data[this->frame_.size++] = b;
    const std::size_t n = this->frame_.size;
    if (n < 2 || n < frame_size(this->frame_.data[1]))
      return;
    if (this->frame_.data[n - 1] != FRAME_END) {
      this->sink_.on_error(Error::ERROR_END_BYTE);
    } else if (checksum(this->frame_.data.data() + 1, n - 3) != this->frame_.data[n - 2]) {
      this->sink_.on_error(Error::ERROR_CHECKSUM);
    } else {
      this->sink_.on_frame(this->frame_);
      this->frame_.size = 0;
      return;
    }
    this->resync_();
  }

  /// Drops the rejected frame and feeds what followed its first inner A8 again. The new start may call for a
  /// shorter frame than the bytes kept, so they go through frame_byte_() one by one.
  constexpr void resync_() {
    const Frame rejected = this->frame_;
    this->frame_.size = 0;
    std::size_t i = 1;
    while (i < rejected.size && rejected.data[i] != FRAME_START)
      ++i;
    for (; i < rejected.size; ++i)
      this->frame_byte_(rejected.data[i]);
  }

  Sink sink_;
  Timing timing_;
  uint64_t now_ = 0;
  uint64_t pending_start_ = 0;
  uint64_t pending_end_ = 0;
  uint64_t low_start_ = 0;
  uint64_t byte_start_ = 0;
  uint64_t byte_end_ = 0;
  uint32_t glitches_ = 0;
  Frame frame_{};
  uint8_t value_ = 0xFF;
  bool in_low_ = false;
  bool pending_ = false;  // a pulse that ended less than merge_gap_ns ago
  bool in_byte_ = false;
};

}  // namespace esphome::scs_sniffer

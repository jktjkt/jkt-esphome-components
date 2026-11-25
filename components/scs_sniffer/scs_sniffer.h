#pragma once

#include "esphome/components/remote_base/remote_base.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"
#include "scs_decoder.h"
#include "scs_protocol.h"

#include <cstdint>
#include <utility>

namespace esphome::scs_sniffer {

/// Decodes the captures of a `remote_receiver` that listens to a passive tap on the BTicino SCS bus.
///
/// Every valid frame is logged at DEBUG and passed to the frame callbacks. A capture with a decoding error, a byte
/// outside a frame other than an A5 acknowledgement, or a glitch is logged at WARN: a summary, the decoded bytes and
/// the raw capture, ready to be replayed with Decoder::feed_marks_spaces().
class SCSSniffer final : public Component, public remote_base::RemoteReceiverListener {
 public:
  SCSSniffer() { this->decoder_.sink().parent = this; }

  /// Driven high at startup: the Grove pin of the RS-485 module that switches its receiver on
  void set_enable_pin(GPIOPin *pin) { this->enable_pin_ = pin; }
  void setup() override;
  void dump_config() override;

  bool on_receive(remote_base::RemoteReceiveData data) override;

  /// The frame with A8 and A3, what it means, and a one line description that is valid during the call only
  template<typename F> void add_on_frame_callback(F &&callback) {
    this->frame_callback_.add(std::forward<F>(callback));
  }

 protected:
  static constexpr size_t MAX_LOGGED_BYTES = 64;
  static constexpr size_t MAX_LOGGED_ERRORS = 8;

  /// What the decoder made of one capture
  struct Sink {
    SCSSniffer *parent{nullptr};
    StaticVector<uint8_t, MAX_LOGGED_BYTES> bytes;
    StaticVector<Error, MAX_LOGGED_ERRORS> errors;
    uint16_t frames{0};
    uint16_t stray{0};
    uint16_t errors_dropped{0};
    bool bytes_dropped{false};

    void reset();
    void on_byte(uint8_t b);
    void on_stray(uint8_t b);
    void on_error(Error e);
    void on_frame(const Frame &f);
  };

  void log_capture_(remote_base::RawTimingsView raw, uint32_t glitches);

  GPIOPin *enable_pin_{nullptr};
  LazyCallbackManager<void(const Frame &, const Message &, StringRef)> frame_callback_;
  Decoder<Sink> decoder_;
};

}  // namespace esphome::scs_sniffer

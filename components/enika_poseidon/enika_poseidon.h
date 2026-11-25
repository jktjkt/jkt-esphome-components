#pragma once

#include "esphome/components/sx126x/sx126x.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/string_ref.h"
#include "poseidon_packet.h"

#include <span>
#include <vector>

namespace esphome::enika_poseidon {

/// Receives Enika Poseidon packets (868.3 MHz, 2-FSK, 38.4 kbit/s) from an sx126x radio.
/// The radio checks the CRC and drops packets that fail it, so every packet seen here has a good CRC.
class PoseidonReceiver final : public Component, public sx126x::SX126xListener {
 public:
  void dump_config() override;

  void on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) override;

  /// Every packet: the frame `<length> <data>` (the radio strips the length byte), RSSI in dBm and the summary line.
  /// The frame and the summary are only valid during the call.
  template<typename F> void add_on_packet_callback(F &&callback) {
    this->packet_callback_.add(std::forward<F>(callback));
  }
  /// Only frames that are transmitter state messages, e.g. key fob presses, and the RSSI in dBm.
  template<typename F> void add_on_state_message_callback(F &&callback) {
    this->state_message_callback_.add(std::forward<F>(callback));
  }

  uint32_t get_packet_count() const { return this->packet_count_; }

 protected:
  LazyCallbackManager<void(std::span<const uint8_t>, float, StringRef)> packet_callback_;
  LazyCallbackManager<void(const StateMessage &, float)> state_message_callback_;
  uint32_t packet_count_{0};
};

}  // namespace esphome::enika_poseidon

#include "enika_poseidon.h"
#include "esphome/core/log.h"

#include <array>
#include <cstring>

namespace esphome::enika_poseidon {

ESPHOME_LOG_TAG(TAG, "enika_poseidon");

// Every packet of the radio fits a frame together with the length byte that the radio strips
static_assert(sx126x::SX126X_MAX_PACKET_SIZE < MAX_FRAME);

void PoseidonReceiver::dump_config() { ESP_LOGCONFIG(TAG, "Enika Poseidon receiver"); }

void PoseidonReceiver::on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) {
  this->packet_count_++;
  std::array<uint8_t, MAX_FRAME> buf;
  buf[0] = packet.size();
  std::memcpy(buf.data() + 1, packet.data(), packet.size());
  const std::span<const uint8_t> frame(buf.data(), packet.size() + 1);

  char summary[SUMMARY_SIZE];
  const size_t summary_len = summarize(frame, summary);
  ESP_LOGD(TAG, "%.0f dBm %s", rssi, summary);
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  char hex[format_hex_pretty_size(MAX_FRAME)];
  ESP_LOGV(TAG, "Frame: %s", format_hex_pretty_to(hex, frame.data(), frame.size(), ' '));
#endif

  this->packet_callback_.call(frame, rssi, StringRef(summary, summary_len));
  StateMessage message;
  if (decode_state_message(frame, message))
    this->state_message_callback_.call(message, rssi);
}

}  // namespace esphome::enika_poseidon

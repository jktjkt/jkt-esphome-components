#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include <esp_err.h>
#include <mbedtls/pk.h>

#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/core/component.h"
#include "esphome/core/string_ref.h"

namespace esphome::per_device_identity {

/// The DER SubjectPublicKeyInfo of a P-256 key is 91 bytes, this leaves room
static constexpr size_t MAX_PUBKEY_DER = 160;

/// Picks the MQTT client identity before the first connect. It runs at the default setup priority (DATA), before
/// mqtt (AFTER_WIFI) hands the credentials to its backend.
///
/// NVS partition `partition_`, namespace "identity":
///   cert    PEM client cert           } both present -> use them
///   key     PEM private key           }
///   pendkey PEM key awaiting a cert (bootstrap mode)
///
/// Without cert and key the bootstrap pair from the mqtt: block stays in place and the device enrols on the broker's
/// enrolment listener (enrol_port_) as client "enrol-<id>": it generates a P-256 key (persisted as pendkey so
/// reboots keep asking for the same key), publishes a CSR to <id>/enrol/csr and waits on <id>/enrol/cert. The ESP32
/// MQTT backend reads the TLS credentials only on the first connect, so a received cert is stored and the device
/// reboots into it.
///
/// <id> is the ESPHome device name with name_add_mac_suffix, <name>-<last 3 MAC bytes>, which ESPHome uses as the
/// MQTT topic prefix too. With broker_domain set, the broker is <id>.<broker_domain> instead of mqtt: broker.
class PerDeviceIdentity final : public Component {
 public:
  explicit PerDeviceIdentity(mqtt::MQTTClientComponent *mqtt) : mqtt_(mqtt) {}

  void set_partition(const char *partition) { this->partition_ = StringRef(partition); }
  void set_csr_interval(uint32_t ms) { this->csr_interval_ = ms; }
  void set_enrol_port(uint16_t port) { this->enrol_port_ = port; }
  void set_broker_domain(const char *domain) { this->broker_domain_ = StringRef(domain); }

  void setup() override;
  void dump_config() override;

  bool is_enrolled() const { return this->enrolled_; }

 protected:
  /// The topic of the cert, <id>/enrol/cert, for the subscription
  static StringRef build_cert_topic_(const void *self, std::span<char, mqtt::MQTT_DEFAULT_TOPIC_MAX_LEN> buf);
  /// <id>.<broker_domain> into buf, cut to fit
  size_t broker_host_(std::span<char> buf) const;
  /// A string from NVS into out, which stays null when it is not there. False on a read error, which is logged.
  bool read_str_(const char *key, std::unique_ptr<char[]> &out) const;
  /// Writes key = value to NVS, or erases key when value is null
  esp_err_t store_(const char *key, const char *value) const;
  bool load_or_generate_key_(mbedtls_pk_context *pk);
  bool prepare_csr_();
  void check_csr_();
  void on_cert_(const std::string &pem);

  mqtt::MQTTClientComponent *mqtt_;
  StringRef partition_;
  StringRef broker_domain_;
  // Handed to the MQTT backend, which keeps only the pointers: never reassign
  std::unique_ptr<char[]> cert_;
  std::unique_ptr<char[]> key_;
  // Bootstrap mode only
  std::unique_ptr<char[]> pending_key_;
  std::unique_ptr<char[]> csr_;
  uint32_t csr_interval_{0};
  /// Time left until the next CSR goes out, counted down while connected
  uint32_t csr_wait_{0};
  std::array<unsigned char, MAX_PUBKEY_DER> pubkey_der_{};
  uint16_t csr_len_{0};
  uint16_t enrol_port_{0};
  uint8_t pubkey_der_len_{0};
  bool enrolled_{false};
};

}  // namespace esphome::per_device_identity

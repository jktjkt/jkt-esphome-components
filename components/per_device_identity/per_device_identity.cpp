#include "per_device_identity.h"

#include <bootloader_random.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <psa/crypto.h>
#include <nvs.h>
#include <nvs_flash.h>

#include <algorithm>
#include <cstring>

#include "esphome/core/application.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::per_device_identity {

ESPHOME_LOG_TAG(TAG, "per_device_identity");

static constexpr const char *NVS_NAMESPACE = "identity";
static constexpr const char *NVS_CERT = "cert";
static constexpr const char *NVS_KEY = "key";
static constexpr const char *NVS_PENDING_KEY = "pendkey";

static constexpr StringRef CERT_TOPIC_SUFFIX = StringRef::from_lit("enrol/cert");
static constexpr StringRef CSR_TOPIC_SUFFIX = StringRef::from_lit("enrol/csr");

/// The longest broker_domain that __init__.py accepts
static constexpr size_t MAX_BROKER_DOMAIN_LEN = 200;
static constexpr size_t BROKER_HOST_SIZE = ESPHOME_DEVICE_NAME_MAX_LEN + 1 + MAX_BROKER_DOMAIN_LEN + 1;
/// A P-256 key is about 230 characters of PEM, its CSR about 500
static constexpr size_t MAX_KEY_PEM = 512;
static constexpr size_t MAX_CSR_PEM = 1024;

static constexpr uint32_t CSR_INTERVAL_ID = 0;
/// How often check_csr_() runs. The first CSR after connecting waits for at least one of these, so that the
/// subscription to the cert topic is acknowledged before the answer can come.
static constexpr uint32_t CSR_CHECK_INTERVAL = 2000;

/// <a><sep><b> into buf; when it does not fit, a is cut and b kept
static size_t join(std::span<char> buf, StringRef a, char sep, StringRef b) {
  return make_name_with_suffix_to(buf.data(), buf.size(), a.c_str(), a.size(), sep, b.c_str(), b.size());
}

/// A new P-256 key pair in the empty pk. mbedTLS 4 makes keys only through PSA, which draws its random numbers from
/// esp_fill_random().
static int generate_p256_key(mbedtls_pk_context *pk) {
  // Made in PSA, copied into the PK context and destroyed in PSA again
  psa_key_attributes_t attributes = psa_key_attributes_init();
  psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
  psa_set_key_bits(&attributes, 256);
  psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_EXPORT);
  mbedtls_svc_key_id_t id{};
  int ret = psa_generate_key(&attributes, &id);
  psa_reset_key_attributes(&attributes);
  if (ret != PSA_SUCCESS)
    return ret;
  ret = mbedtls_pk_copy_from_psa(id, pk);
  psa_destroy_key(id);
  return ret;
}

/// The DER SubjectPublicKeyInfo of pk at the start of buf. Its length, 0 on failure.
static size_t pubkey_der(mbedtls_pk_context *pk, std::span<unsigned char, MAX_PUBKEY_DER> buf) {
  const int len = mbedtls_pk_write_pubkey_der(pk, buf.data(), buf.size());
  if (len <= 0)
    return 0;
  // mbedTLS writes it at the end of the buffer
  memmove(buf.data(), buf.data() + buf.size() - len, len);
  return len;
}

/// A PEM that write() puts into a new buffer of size, NUL terminated
template<typename Write> static int write_pem(std::unique_ptr<char[]> &out, size_t size, Write write) {
  out = std::make_unique<char[]>(size);
  const int ret = write(reinterpret_cast<unsigned char *>(out.get()), size);
  if (ret != 0) {
    // What a failed write left may be part of a key
    mbedtls_platform_zeroize(out.get(), size);
    out.reset();
  }
  return ret;
}

StringRef PerDeviceIdentity::build_cert_topic_(const void * /*self*/,
                                               std::span<char, mqtt::MQTT_DEFAULT_TOPIC_MAX_LEN> buf) {
  return StringRef(buf.data(), join(buf, App.get_name(), '/', CERT_TOPIC_SUFFIX));
}

size_t PerDeviceIdentity::broker_host_(std::span<char> buf) const {
  return join(buf, App.get_name(), '.', this->broker_domain_);
}

bool PerDeviceIdentity::read_str_(const char *key, std::unique_ptr<char[]> &out) const {
  nvs_handle_t h;
  esp_err_t err = nvs_open_from_partition(this->partition_.c_str(), NVS_NAMESPACE, NVS_READONLY, &h);
  if (err == ESP_OK) {
    size_t len = 0;
    err = nvs_get_str(h, key, nullptr, &len);
    if (err == ESP_OK && len > 1) {
      out = std::make_unique<char[]>(len);
      err = nvs_get_str(h, key, out.get(), &len);
      if (err != ESP_OK)
        out.reset();
    }
    nvs_close(h);
  }
  // A namespace or key that is not there yet is no error
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGE(TAG, "Reading '%s' failed: %s", key, esp_err_to_name(err));
    return false;
  }
  return true;
}

esp_err_t PerDeviceIdentity::store_(const char *key, const char *value) const {
  nvs_handle_t h;
  esp_err_t err = nvs_open_from_partition(this->partition_.c_str(), NVS_NAMESPACE, NVS_READWRITE, &h);
  if (err != ESP_OK)
    return err;
  err = value != nullptr ? nvs_set_str(h, key, value) : nvs_erase_key(h, key);
  if (err == ESP_OK)
    err = nvs_commit(h);
  nvs_close(h);
  return err;
}

void PerDeviceIdentity::setup() {
  const StringRef &id = App.get_name();
  // The device listener forces the client id to the cert CN anyway. The client keeps a copy.
  this->mqtt_->set_client_id(id.str());
  if (!this->broker_domain_.empty()) {
    // ESPHome hands the name, not the address, to esp-tls, so the broker cert needs a matching SAN
    char host[BROKER_HOST_SIZE];
    this->mqtt_->set_broker_address(std::string(host, this->broker_host_(host)));
  }

  const esp_err_t err = nvs_flash_init_partition(this->partition_.c_str());
  if (err != ESP_OK) {
    // No auto-erase: that could destroy an enrolled key
    ESP_LOGE(TAG, "Opening NVS partition '%s' failed: %s", this->partition_.c_str(), esp_err_to_name(err));
    this->mark_failed(LOG_STR("NVS partition unavailable"));
    return;
  }

  // A cert or key that cannot be read must not lead to a new enrolment, which would overwrite them
  std::unique_ptr<char[]> cert, key;
  if (!this->read_str_(NVS_CERT, cert) || !this->read_str_(NVS_KEY, key)) {
    this->mark_failed(LOG_STR("Reading the identity failed"));
    return;
  }
  if (cert && key) {
    this->cert_ = std::move(cert);
    this->key_ = std::move(key);
    this->mqtt_->set_cl_certificate(this->cert_.get());
    this->mqtt_->set_cl_key(this->key_.get());
    this->enrolled_ = true;
    return;
  }

  if (!this->prepare_csr_()) {
    this->mark_failed(LOG_STR("Preparing the CSR failed"));
    return;
  }
  // The client id differs from an enrolled device's, which is forced to its cert CN, so that it never kicks it off
  char client_id[sizeof("enrol-") + ESPHOME_DEVICE_NAME_MAX_LEN];
  this->mqtt_->set_client_id(std::string(client_id, join(client_id, StringRef::from_lit("enrol"), '-', id)));
  this->mqtt_->set_broker_port(this->enrol_port_);
  // The topic is built when the client needs it, and the callback captures only this: nothing is allocated
  this->mqtt_->subscribe(
      mqtt::MQTTTopicSource{&PerDeviceIdentity::build_cert_topic_, this},
      [this](const std::string &, const std::string &payload) { this->on_cert_(payload); }, 1);
  this->csr_wait_ = 2 * CSR_CHECK_INTERVAL;
  this->set_interval(CSR_INTERVAL_ID, CSR_CHECK_INTERVAL, [this]() { this->check_csr_(); });
}

bool PerDeviceIdentity::load_or_generate_key_(mbedtls_pk_context *pk) {
  if (!this->read_str_(NVS_PENDING_KEY, this->pending_key_))
    return false;
  if (this->pending_key_) {
    // The length of a PEM includes its NUL
    const auto *pem = reinterpret_cast<const unsigned char *>(this->pending_key_.get());
    const int ret = mbedtls_pk_parse_key(pk, pem, strlen(this->pending_key_.get()) + 1, nullptr, 0);
    if (ret != 0) {
      ESP_LOGE(TAG, "Parsing the pending key failed: -0x%04x", static_cast<unsigned>(-ret));
    }
    return ret == 0;
  }

  ESP_LOGI(TAG, "Generating P-256 key");
  // The P4 RNG only has a true entropy source while the SAR ADC noise path is on
  bootloader_random_enable();
  int ret = generate_p256_key(pk);
  bootloader_random_disable();
  if (ret == 0) {
    ret = write_pem(this->pending_key_, MAX_KEY_PEM,
                    [pk](unsigned char *buf, size_t size) { return mbedtls_pk_write_key_pem(pk, buf, size); });
  }
  if (ret != 0) {
    ESP_LOGE(TAG, "Generating the key failed: -0x%04x", static_cast<unsigned>(-ret));
    return false;
  }
  // Kept so that reboots keep asking for the same key
  const esp_err_t err = this->store_(NVS_PENDING_KEY, this->pending_key_.get());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Storing the key failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool PerDeviceIdentity::prepare_csr_() {
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  if (!this->load_or_generate_key_(&pk)) {
    mbedtls_pk_free(&pk);
    return false;
  }

  mbedtls_x509write_csr csr;
  mbedtls_x509write_csr_init(&csr);
  mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
  mbedtls_x509write_csr_set_key(&csr, &pk);
  char subject[sizeof("CN=") + ESPHOME_DEVICE_NAME_MAX_LEN];
  join(subject, StringRef::from_lit("CN"), '=', App.get_name());
  int ret = mbedtls_x509write_csr_set_subject_name(&csr, subject);
  if (ret == 0) {
    ret = write_pem(this->csr_, MAX_CSR_PEM,
                    [&csr](unsigned char *buf, size_t size) { return mbedtls_x509write_csr_pem(&csr, buf, size); });
  }
  if (ret == 0) {
    this->csr_len_ = strlen(this->csr_.get());
    this->pubkey_der_len_ = pubkey_der(&pk, this->pubkey_der_);
    if (this->pubkey_der_len_ == 0)
      ret = -1;
  }
  if (ret != 0) {
    ESP_LOGE(TAG, "Generating the CSR failed: -0x%04x", static_cast<unsigned>(-ret));
  }
  mbedtls_x509write_csr_free(&csr);
  mbedtls_pk_free(&pk);
  return ret == 0;
}

void PerDeviceIdentity::check_csr_() {
  if (!this->mqtt_->is_connected()) {
    this->csr_wait_ = 2 * CSR_CHECK_INTERVAL;
    return;
  }
  if (this->csr_wait_ > CSR_CHECK_INTERVAL) {
    this->csr_wait_ -= CSR_CHECK_INTERVAL;
    return;
  }
  this->csr_wait_ = this->csr_interval_;
  char topic[ESPHOME_DEVICE_NAME_MAX_LEN + 1 + CSR_TOPIC_SUFFIX.size() + 1];
  join(topic, App.get_name(), '/', CSR_TOPIC_SUFFIX);
  ESP_LOGD(TAG, "Requesting a cert");
  if (!this->mqtt_->publish(topic, this->csr_.get(), this->csr_len_, 1, false)) {
    ESP_LOGW(TAG, "Publishing the CSR failed");
  }
}

void PerDeviceIdentity::on_cert_(const std::string &pem) {
  // Another cert can come before the reboot
  if (this->enrolled_)
    return;
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  const int ret = mbedtls_x509_crt_parse(&crt, reinterpret_cast<const unsigned char *>(pem.c_str()), pem.size() + 1);
  std::array<unsigned char, MAX_PUBKEY_DER> buf;
  const size_t len = ret == 0 ? pubkey_der(&crt.pk, buf) : 0;
  mbedtls_x509_crt_free(&crt);
  if (len == 0 || !std::equal(buf.begin(), buf.begin() + len, this->pubkey_der_.begin(),
                              this->pubkey_der_.begin() + this->pubkey_der_len_)) {
    ESP_LOGW(TAG, "Ignoring a cert that does not match the key (parse: -0x%04x)", static_cast<unsigned>(-ret));
    return;
  }

  // Without both, a reboot comes back to bootstrap mode with the pending key, which is still stored
  esp_err_t err = this->store_(NVS_CERT, pem.c_str());
  if (err == ESP_OK)
    err = this->store_(NVS_KEY, this->pending_key_.get());
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Storing the cert failed: %s", esp_err_to_name(err));
    this->status_set_warning();
    return;
  }
  // A leftover pendkey does no harm: it is not read while cert and key are there
  this->store_(NVS_PENDING_KEY, nullptr);
  this->enrolled_ = true;
  this->cancel_interval(CSR_INTERVAL_ID);
  ESP_LOGI(TAG, "Enrolled, rebooting to connect with the device cert");
  // Gives the log a moment to go out
  this->set_timeout(500, []() { App.safe_reboot(); });
}

void PerDeviceIdentity::dump_config() {
  char host[BROKER_HOST_SIZE];
  const char *broker = LOG_STR_LITERAL("from mqtt:");
  if (!this->broker_domain_.empty()) {
    this->broker_host_(host);
    broker = host;
  }
  ESP_LOGCONFIG(TAG,
                "Per-device identity:\n"
                "  Partition: %s\n"
                "  Device ID: %s\n"
                "  Broker: %s\n"
                "  Mode: %s",
                this->partition_.c_str(), App.get_name().c_str(), broker,
                this->enrolled_ ? LOG_STR_LITERAL("device cert") : LOG_STR_LITERAL("bootstrap (enrolling)"));
}

}  // namespace esphome::per_device_identity

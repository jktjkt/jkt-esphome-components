#include "feature_gate.h"

#include <cinttypes>

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::feature_gate {

ESPHOME_LOG_TAG(TAG, "feature_gate");

static constexpr uint32_t APPLY_TIMEOUT_ID = 0;

/// The longest JSON of the active set: {"name":1,...} for every feature, and the NUL
static constexpr size_t MAX_JSON_LEN = 2 + MAX_FEATURES * (MAX_NAME_LEN + 5) + 1;

int FeatureGate::find_(std::string_view name) const {
  for (size_t i = 0; i < this->features_.size(); i++) {
    if (name == this->features_[i].name)
      return i;
  }
  return -1;
}

bool FeatureGate::is_enabled(std::string_view name) const {
  const int i = this->find_(name);
  return i >= 0 && this->is_active_(i);
}

void FeatureGate::setup() {
  this->pref_ = global_preferences->make_preference<uint32_t>(this->preference_key_, true);
  this->pref_.load(&this->active_mask_);
  this->wanted_mask_ = this->active_mask_;
  for (size_t i = 0; i < this->features_.size(); i++) {
    if (this->is_active_(i))
      continue;
    const Feature &f = this->features_[i];
    for (Component *c : f.components) {
      if ((c->get_component_state() & COMPONENT_STATE_MASK) != COMPONENT_STATE_CONSTRUCTION) {
        // Its setup priority is above ours; it has already run.
        ESP_LOGE(TAG, "Feature %s: %s is already set up, cannot disable it", f.name,
                 LOG_STR_ARG(c->get_component_log_str()));
        continue;
      }
      // LOOP_DONE before setup: Component::call() neither sets it up nor loops it.
      c->disable_loop();
    }
  }
}

void FeatureGate::loop() {
  // The topic prefix is final only once every component is set up (per_device_identity may change it), which is
  // guaranteed once MQTT is connected. The topic is built when the client needs it, so nothing is stored for it.
  if (!this->mqtt_->is_connected())
    return;
  mqtt::MQTTTopicSource config_topic{[](const void *self, std::span<char, mqtt::MQTT_DEFAULT_TOPIC_MAX_LEN> buf) {
                                       const auto *gate = static_cast<const FeatureGate *>(self);
                                       const size_t len = buf_append_printf(buf.data(), buf.size(), 0, "%s/%s/+",
                                                                            gate->mqtt_->get_topic_prefix().c_str(),
                                                                            gate->topic_.c_str());
                                       return StringRef(buf.data(), len);
                                     },
                                     this};
  this->mqtt_->subscribe(
      config_topic, [this](const std::string &topic, const std::string &payload) { this->on_config_(topic, payload); },
      1);
  this->publish_active_();
  this->disable_loop();
}

void FeatureGate::on_config_(const std::string &topic, const std::string &payload) {
  const std::string_view name = std::string_view(topic).substr(topic.rfind('/') + 1);
  const int i = this->find_(name);
  if (i < 0) {
    ESP_LOGW(TAG, "No feature %.*s, ignoring %s", static_cast<int>(name.size()), name.data(), topic.c_str());
    return;
  }
  const char *feature = this->features_[i].name;
  bool on;
  if (payload.empty()) {
    // The retained message was cleared
    on = this->default_mask_ & (1u << i);
  } else if (payload == "1" || payload == "true" || payload == "on") {
    on = true;
  } else if (payload == "0" || payload == "false" || payload == "off") {
    on = false;
  } else {
    ESP_LOGW(TAG, "Feature %s: ignoring '%s'", feature, payload.c_str());
    return;
  }
  assign_bits(this->wanted_mask_, 1u << i, on);
  if (this->wanted_mask_ == this->active_mask_) {
    this->cancel_timeout(APPLY_TIMEOUT_ID);
    return;
  }
  ESP_LOGI(TAG, "Feature %s -> %s; rebooting in %" PRIu32 " ms unless reverted", feature,
           on ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"), this->reboot_delay_);
  this->set_timeout(APPLY_TIMEOUT_ID, this->reboot_delay_, [this]() { this->apply_(); });
}

void FeatureGate::apply_() {
  if (!this->pref_.save(&this->wanted_mask_) || !global_preferences->sync()) {
    // Rebooting now would come back with the old set and loop.
    ESP_LOGE(TAG, "Storing the features failed; not rebooting");
    return;
  }
  ESP_LOGI(TAG, "Features stored, rebooting");
  App.safe_reboot();
}

void FeatureGate::publish_active_() {
  char topic[mqtt::MQTT_DEFAULT_TOPIC_MAX_LEN];
  buf_append_printf(topic, sizeof(topic), 0, "%s/features", this->mqtt_->get_topic_prefix().c_str());
  // The names need no escaping, they are [a-z0-9_]
  char json[MAX_JSON_LEN];
  size_t len = buf_append_str(json, sizeof(json), 0, "{");
  for (size_t i = 0; i < this->features_.size(); i++) {
    len = buf_append_printf(json, sizeof(json), len, "%s\"%s\":%c", i == 0 ? "" : ",", this->features_[i].name,
                            this->is_active_(i) ? '1' : '0');
  }
  len = buf_append_str(json, sizeof(json), len, "}");
  if (!this->mqtt_->publish(topic, json, len, 1, true))
    ESP_LOGW(TAG, "Publishing the active features failed");
}

void FeatureGate::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Feature gate:\n"
                "  Topic: %s\n"
                "  Reboot delay: %" PRIu32 " ms",
                this->topic_.c_str(), this->reboot_delay_);
  for (size_t i = 0; i < this->features_.size(); i++) {
    const Feature &f = this->features_[i];
    ESP_LOGCONFIG(TAG, "  %s: %s (%zu components)", f.name,
                  this->is_active_(i) ? LOG_STR_LITERAL("on") : LOG_STR_LITERAL("off"), f.components.size());
  }
}

}  // namespace esphome::feature_gate

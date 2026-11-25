#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/core/string_ref.h"

namespace esphome::feature_gate {

/// The state of the features is a bit mask of a uint32_t; the code generator checks the count
static constexpr size_t MAX_FEATURES = 32;
/// The longest feature name, checked by the code generator
static constexpr size_t MAX_NAME_LEN = 15;
/// The longest `topic`, checked by the code generator
static constexpr size_t MAX_TOPIC_LEN = 64;
static_assert(mqtt::MQTT_TOPIC_PREFIX_MAX_LEN + sizeof("/") + MAX_TOPIC_LEN + sizeof("/+") <=
              mqtt::MQTT_DEFAULT_TOPIC_MAX_LEN);

/// The element of a feature's table of components. The alias makes `static const ComponentPtr x[]` a table of const
/// pointers to components that can be parked, which `static const Component *x[]` is not.
using ComponentPtr = Component *;

/// A feature as the configuration has it. The code generator puts them all into a static table.
struct Feature {
  const char *name;
  std::span<const ComponentPtr> components;
};

/// Per-device feature switches for a single firmware image.
///
/// Each feature is a group of components. At boot, before any of them is set up, the gate loads the set of enabled
/// features from the preferences (the YAML defaults if nothing is stored) and parks the components of disabled
/// features in LOOP_DONE, so App::setup() never calls their setup() and the main loop never calls loop().
///
/// After MQTT connects it subscribes to <topic_prefix>/<topic>/+ (retained, "1"/"0", empty = default) and publishes
/// the active set to <topic_prefix>/features. A value that differs from the active one is stored and the device
/// reboots: ESPHome has no way to tear a component down, and setting one up late breaks the setup order its
/// dependencies rely on.
class FeatureGate final : public Component {
 public:
  /// `preference_key` is derived from the feature names, so a changed list does not load a stale set
  FeatureGate(mqtt::MQTTClientComponent *mqtt, const Feature *features, size_t count, uint32_t default_mask,
              uint32_t preference_key)
      : mqtt_(mqtt),
        features_(features, count),
        default_mask_(default_mask),
        active_mask_(default_mask),
        wanted_mask_(default_mask),
        preference_key_(preference_key) {}
  void set_topic(const char *topic) { this->topic_ = StringRef(topic); }
  void set_reboot_delay(uint32_t ms) { this->reboot_delay_ = ms; }

  /// For lambdas: is the feature running in this boot? False for an unknown name.
  bool is_enabled(std::string_view name) const;

  void setup() override;
  void loop() override;
  void dump_config() override;
  // Same as the logger: ahead of every component it might gate, including buses and POWER
  float get_setup_priority() const override { return setup_priority::BUS + 500.0f; }

 protected:
  /// The index of the feature with this name, or -1
  int find_(std::string_view name) const;
  void on_config_(const std::string &topic, const std::string &payload);
  void apply_();
  void publish_active_();
  bool is_active_(size_t i) const { return this->active_mask_ & (1u << i); }

  mqtt::MQTTClientComponent *mqtt_;
  std::span<const Feature> features_;
  StringRef topic_;
  ESPPreferenceObject pref_;
  uint32_t reboot_delay_{0};
  uint32_t default_mask_;
  uint32_t active_mask_;  // this boot
  uint32_t wanted_mask_;  // latest from MQTT
  uint32_t preference_key_;
};

}  // namespace esphome::feature_gate

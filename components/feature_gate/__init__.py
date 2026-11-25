"""Turn groups of compiled-in components on or off per device, from retained
MQTT topics <topic_prefix>/<topic>/<feature> ("1"/"0"). The choice is stored in
the preferences and applied at boot, before the components' setup(); a change
reboots."""

import voluptuous as vol

import esphome.codegen as cg
from esphome.components import mqtt
import esphome.config_validation as cv
from esphome.const import CONF_COMPONENTS, CONF_DEFAULT, CONF_ID, CONF_NAME, CONF_TOPIC
from esphome.helpers import fnv1_hash
from esphome.types import ConfigType

DEPENDENCIES = ["mqtt"]
DOMAIN = "feature_gate"

CONF_COMPONENTS_ID = "components_id"
CONF_FEATURES = "features"
CONF_FEATURES_ID = "features_id"
CONF_MQTT_ID = "mqtt_id"
CONF_REBOOT_DELAY = "reboot_delay"

# MAX_FEATURES, MAX_NAME_LEN and MAX_TOPIC_LEN in feature_gate.h
MAX_FEATURES = 32
MAX_NAME_LEN = 15
MAX_TOPIC_LEN = 64

feature_gate_ns = cg.esphome_ns.namespace("feature_gate")
FeatureGate = feature_gate_ns.class_("FeatureGate", cg.Component)
Feature = feature_gate_ns.struct("Feature")
ComponentPtr = feature_gate_ns.class_("ComponentPtr")

_NAME = vol.Match(r"^[a-z0-9_]+$")

FEATURE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.All(
            cv.string, cv.Length(min=1, max=MAX_NAME_LEN), _NAME
        ),
        cv.Optional(CONF_DEFAULT, default=False): cv.boolean,
        # Anything that calls into a gated component must be gated too, since gated components never run setup().
        cv.Required(CONF_COMPONENTS): cv.All(
            cv.ensure_list(cv.use_id(cg.Component)), cv.Length(min=1)
        ),
        cv.GenerateID(CONF_COMPONENTS_ID): cv.declare_id(ComponentPtr),
    }
)


def _unique_names(features: list[ConfigType]) -> list[ConfigType]:
    names = [f[CONF_NAME] for f in features]
    if len(names) != len(set(names)):
        raise cv.Invalid("Feature names must be unique")
    return features


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(FeatureGate),
        cv.GenerateID(CONF_MQTT_ID): cv.use_id(mqtt.MQTTClientComponent),
        cv.Optional(CONF_TOPIC, default="config"): cv.All(
            cv.string, cv.Length(max=MAX_TOPIC_LEN), _NAME
        ),
        # Retained messages for all features arrive back to back after connecting; wait for the burst to end
        # before rebooting once.
        cv.Optional(
            CONF_REBOOT_DELAY, default="3s"
        ): cv.positive_time_period_milliseconds,
        cv.Required(CONF_FEATURES): cv.All(
            cv.ensure_list(FEATURE_SCHEMA),
            cv.Length(min=1, max=MAX_FEATURES),
            _unique_names,
        ),
        cv.GenerateID(CONF_FEATURES_ID): cv.declare_id(Feature),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config: ConfigType) -> None:
    features = config[CONF_FEATURES]
    rows = []
    for feature in features:
        components = [
            await cg.get_variable(comp_id) for comp_id in feature[CONF_COMPONENTS]
        ]
        table = cg.static_const_array(
            feature[CONF_COMPONENTS_ID], cg.ArrayInitializer(*components)
        )
        rows.append(cg.ArrayInitializer(feature[CONF_NAME], table))
    table = cg.static_const_array(
        config[CONF_FEATURES_ID], cg.ArrayInitializer(*rows, multiline=True)
    )
    default_mask = sum(1 << i for i, f in enumerate(features) if f[CONF_DEFAULT])
    # A changed list of features gets a new slot instead of loading a set whose bits mean other features
    preference_key = fnv1_hash(
        "feature_gate:" + ",".join(f[CONF_NAME] for f in features)
    )
    var = cg.new_Pvariable(
        config[CONF_ID],
        await cg.get_variable(config[CONF_MQTT_ID]),
        table,
        len(features),
        default_mask,
        preference_key,
    )
    await cg.register_component(var, config)
    cg.add(var.set_topic(config[CONF_TOPIC]))
    cg.add(var.set_reboot_delay(config[CONF_REBOOT_DELAY]))

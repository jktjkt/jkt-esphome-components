"""Per-device MQTT identity for a single firmware image: device id from the
MAC suffix, per-device broker address, client cert from a dedicated NVS
partition, and CSR-over-MQTT enrolment using the bootstrap cert from mqtt:."""

import voluptuous as vol

import esphome.codegen as cg
from esphome.components import mqtt
from esphome.components.esp32 import add_idf_sdkconfig_option, require_idf_version
import esphome.config_validation as cv
from esphome.const import (
    CONF_ESPHOME,
    CONF_ID,
    CONF_MQTT,
    CONF_MQTT_ID,
    CONF_NAME,
    CONF_NAME_ADD_MAC_SUFFIX,
    CONF_TOPIC_PREFIX,
)
import esphome.final_validate as fv
from esphome.types import ConfigType

DEPENDENCIES = ["esp32", "mqtt"]
DOMAIN = "per_device_identity"

CONF_PARTITION = "partition"
CONF_CSR_INTERVAL = "csr_interval"
CONF_ENROL_PORT = "enrol_port"
CONF_BROKER_DOMAIN = "broker_domain"

# Keep in sync with MAX_BROKER_DOMAIN_LEN in per_device_identity.cpp
MAX_BROKER_DOMAIN_LEN = 200

per_device_identity_ns = cg.esphome_ns.namespace("per_device_identity")
PerDeviceIdentity = per_device_identity_ns.class_("PerDeviceIdentity", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PerDeviceIdentity),
            cv.GenerateID(CONF_MQTT_ID): cv.use_id(mqtt.MQTTClientComponent),
            cv.Optional(CONF_PARTITION, default="identity"): cv.All(
                cv.string, cv.Length(max=15)
            ),
            cv.Optional(
                CONF_CSR_INTERVAL, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ENROL_PORT, default=8884): cv.port,
            cv.Optional(CONF_BROKER_DOMAIN): cv.All(
                cv.string_strict,
                cv.Length(max=MAX_BROKER_DOMAIN_LEN),
                vol.Match(r"^[a-z0-9-]+(\.[a-z0-9-]+)+$"),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    # The key generation and the CSR use the API of mbedTLS 4
    require_idf_version(cv.Version(6, 0, 0), "per_device_identity"),
)


def _final_validate(config: ConfigType) -> ConfigType:
    full = fv.full_config.get()
    esphome = full[CONF_ESPHOME]
    # The device id (cert CN, client id, topic prefix) is <name>-<last 3 MAC bytes>
    if not esphome.get(CONF_NAME_ADD_MAC_SUFFIX):
        raise cv.Invalid(
            f"per_device_identity needs '{CONF_NAME_ADD_MAC_SUFFIX}: true' under esphome:"
        )
    # A broker ACL that pins each device to <cert CN>/# needs the topics there
    if full[CONF_MQTT].get(CONF_TOPIC_PREFIX, esphome[CONF_NAME]) != esphome[CONF_NAME]:
        raise cv.Invalid(
            f"per_device_identity needs the default mqtt {CONF_TOPIC_PREFIX} (the device name)"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    mqtt_client = await cg.get_variable(config[CONF_MQTT_ID])
    var = cg.new_Pvariable(config[CONF_ID], mqtt_client)
    await cg.register_component(var, config)
    cg.add(var.set_partition(config[CONF_PARTITION]))
    cg.add(var.set_csr_interval(config[CONF_CSR_INTERVAL]))
    cg.add(var.set_enrol_port(config[CONF_ENROL_PORT]))
    cg.add_optional(var.set_broker_domain, config.get(CONF_BROKER_DOMAIN))
    # ESP-IDF builds the CSR writer only on request
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_X509_CREATE_C", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_X509_CSR_WRITE_C", True)

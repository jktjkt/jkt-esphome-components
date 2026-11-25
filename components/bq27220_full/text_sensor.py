from esphome.automation import parent_ref
import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC
from esphome.types import ConfigType

from . import (
    BQ27220_FULL_CHILD_SCHEMA,
    CONF_BQ27220_FULL_ID,
    FLAGS,
    READING,
    STATUS_PUBLISHER,
    add_dump,
    bq27220_full_ns,
)

DEPENDENCIES = ["bq27220_full"]

CONF_ACTIVE_FLAGS = "active_flags"
CONF_DEVICE_NUMBER = "device_number"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_HARDWARE_VERSION = "hardware_version"
CONF_SECURITY_ACCESS = "security_access"

FlagDef = bq27220_full_ns.struct("FlagDef")

# FlagDef::name holds this many characters, and the C++ buffer of the list this many in all
FLAG_NAME_LENGTH = 9
ACTIVE_FLAGS_LENGTH = 255
assert all(len(name) <= FLAG_NAME_LENGTH for *_, name in FLAGS)
assert sum(len(name) + 1 for *_, name in FLAGS) - 1 <= ACTIVE_FLAGS_LENGTH

_SCHEMA = text_sensor.text_sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC)

# device_number, firmware_version and hardware_version are read once at boot through the
# DEVICE_NUMBER, FW_VERSION and HW_VERSION subcommands. security_access comes from
# OperationStatus()[SEC1:0], active_flags lists every set flag by its TRM name.
CONFIG_SCHEMA = cv.All(
    BQ27220_FULL_CHILD_SCHEMA.extend(
        {
            cv.Optional(CONF_DEVICE_NUMBER): _SCHEMA,
            cv.Optional(CONF_FIRMWARE_VERSION): _SCHEMA,
            cv.Optional(CONF_HARDWARE_VERSION): _SCHEMA,
            cv.Optional(CONF_SECURITY_ACCESS): _SCHEMA,
            cv.Optional(CONF_ACTIVE_FLAGS): _SCHEMA,
        }
    ),
    cv.has_at_least_one_key(
        CONF_DEVICE_NUMBER,
        CONF_FIRMWARE_VERSION,
        CONF_HARDWARE_VERSION,
        CONF_SECURITY_ACCESS,
        CONF_ACTIVE_FLAGS,
    ),
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_BQ27220_FULL_ID])
    sensors = text_sensor.sub_text_sensors(config)
    await sensors(CONF_DEVICE_NUMBER, hub.set_device_number_text_sensor)
    await sensors(CONF_FIRMWARE_VERSION, hub.set_firmware_version_text_sensor)
    await sensors(CONF_HARDWARE_VERSION, hub.set_hardware_version_text_sensor)
    if (var := await sensors(CONF_SECURITY_ACCESS, None)) is not None:
        STATUS_PUBLISHER.add(
            hub, bq27220_full_ns.publish_security_access(parent_ref(var), READING)
        )
        add_dump(
            hub, text_sensor.text_sensor_ns.log_text_sensor, CONF_SECURITY_ACCESS, var
        )
    if (var := await sensors(CONF_ACTIVE_FLAGS, None)) is not None:
        # One table in flash for every gauge
        flags = cg.shared_progmem_array(
            "bq27220_full_flags",
            FlagDef,
            cg.ArrayInitializer(
                *[cg.ArrayInitializer(name, word, bit) for word, bit, name in FLAGS],
                multiline=True,
            ),
        )
        cg.add(hub.set_read_gauging_status(True))
        STATUS_PUBLISHER.add(
            hub, bq27220_full_ns.publish_active_flags(parent_ref(var), READING, flags)
        )
        add_dump(
            hub, text_sensor.text_sensor_ns.log_text_sensor, CONF_ACTIVE_FLAGS, var
        )

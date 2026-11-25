from esphome.automation import parent_ref
import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import DEVICE_CLASS_PROBLEM, ENTITY_CATEGORY_DIAGNOSTIC
from esphome.types import ConfigType

from . import (
    BQ27220_FULL_CHILD_SCHEMA,
    CONF_BQ27220_FULL_ID,
    FLAGS,
    READING,
    STATUS_PUBLISHER,
    WORD_GAUGING,
    add_dump,
    bq27220_full_ns,
)

DEPENDENCIES = ["bq27220_full"]

_PROBLEM = {"SYSDWN", "OTD", "OTC", "OCVFAIL", "CHGINH"}

# Each flag is a binary sensor named after it in lower case. Bits are in SLUUBD4A tables 2-3, 2-4, 2-6 and 2-7
CONFIG_SCHEMA = cv.All(
    BQ27220_FULL_CHILD_SCHEMA.extend(
        {
            cv.Optional(name.lower()): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM if name in _PROBLEM else cv.UNDEFINED,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            )
            for _, _, name in FLAGS
        }
    ),
    cv.has_at_least_one_key(*(name.lower() for _, _, name in FLAGS)),
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_BQ27220_FULL_ID])
    sensors = binary_sensor.sub_binary_sensors(config)
    gauging = False
    for word, bit, name in FLAGS:
        key = name.lower()
        if (var := await sensors(key, None)) is None:
            continue
        gauging |= word is WORD_GAUGING
        STATUS_PUBLISHER.add(
            hub, bq27220_full_ns.publish_flag(parent_ref(var), READING, word, bit)
        )
        add_dump(hub, binary_sensor.binary_sensor_ns.log_binary_sensor, key, var)
    if gauging:
        cg.add(hub.set_read_gauging_status(True))

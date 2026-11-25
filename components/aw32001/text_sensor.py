from esphome.automation import parent_ref
import esphome.codegen as cg
from esphome.components import text_sensor
import esphome.config_validation as cv
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC
from esphome.types import ConfigType

from . import (
    AW32001_CHILD_SCHEMA,
    CONF_AW32001_ID,
    FAULT,
    STATUS,
    STATUS_PUBLISHER,
    aw32001_ns,
)

DEPENDENCIES = ["aw32001"]

CONF_CHARGE_STATUS = "charge_status"
CONF_FAULTS = "faults"

CONFIG_SCHEMA = cv.All(
    AW32001_CHILD_SCHEMA.extend(
        {
            cv.Optional(key): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            )
            for key in (CONF_CHARGE_STATUS, CONF_FAULTS)
        }
    ),
    cv.has_at_least_one_key(CONF_CHARGE_STATUS, CONF_FAULTS),
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_AW32001_ID])
    sensors = text_sensor.sub_text_sensors(config)
    if (var := await sensors(CONF_CHARGE_STATUS, None)) is not None:
        STATUS_PUBLISHER.add(
            hub, aw32001_ns.publish_charge_status(parent_ref(var), STATUS)
        )
    if (var := await sensors(CONF_FAULTS, None)) is not None:
        STATUS_PUBLISHER.add(
            hub, aw32001_ns.publish_faults(parent_ref(var), STATUS, FAULT)
        )

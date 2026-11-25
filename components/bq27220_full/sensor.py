from esphome.automation import parent_ref
import esphome.codegen as cg
from esphome.components import sensor
from esphome.components.const import UNIT_MILLIAMPERE_HOUR
import esphome.config_validation as cv
from esphome.const import (
    ENTITY_CATEGORY_DIAGNOSTIC,
    ICON_BATTERY,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
)
from esphome.core import CORE
from esphome.cpp_generator import HexIntLiteral
import esphome.final_validate as fv
from esphome.types import ConfigType

from . import (
    BQ27220_FULL_CHILD_SCHEMA,
    CONF_AVERAGE_POWER_SCALE,
    CONF_BQ27220_FULL_ID,
    READING,
    STATUS_PUBLISHER,
    add_dump,
    bq27220_full_ns,
)

DEPENDENCIES = ["bq27220_full"]

CONF_AVERAGE_POWER = "average_power"

Conversion = bq27220_full_ns.enum("Conversion", is_class=True)
RAW = Conversion.CONVERSION_RAW
MILLI = Conversion.CONVERSION_MILLI
MILLI_SIGNED = Conversion.CONVERSION_MILLI_SIGNED
MILLI_SENTINEL = Conversion.CONVERSION_MILLI_SENTINEL  # 0xFFFF means "not applicable"
DECI_KELVIN = Conversion.CONVERSION_DECI_KELVIN
SENTINEL = Conversion.CONVERSION_SENTINEL  # 0xFFFF means "not applicable"
LOW3 = Conversion.CONVERSION_LOW3
SIGNED = Conversion.CONVERSION_SIGNED

DIAG = ENTITY_CATEGORY_DIAGNOSTIC


def _voltage() -> cv.Schema:
    return sensor.sensor_schema(
        kind=sensor.VOLTAGE, accuracy_decimals=3, entity_category=DIAG
    )


def _current() -> cv.Schema:
    return sensor.sensor_schema(
        kind=sensor.CURRENT, accuracy_decimals=3, entity_category=DIAG
    )


def _temperature() -> cv.Schema:
    return sensor.sensor_schema(kind=sensor.TEMPERATURE, entity_category=DIAG)


def _mah() -> cv.Schema:
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_MILLIAMPERE_HOUR,
        accuracy_decimals=0,
        state_class=STATE_CLASS_MEASUREMENT,
        icon=ICON_BATTERY,
        entity_category=DIAG,
    )


def _minutes() -> cv.Schema:
    return sensor.sensor_schema(kind=sensor.DURATION_MIN, entity_category=DIAG)


# key -> (register, conversion, schema). Register meanings are in SLUUBD4A table 2-1
SENSORS = {
    "voltage": (0x08, MILLI, _voltage()),
    "current": (0x0C, MILLI_SIGNED, _current()),
    "average_current": (0x14, MILLI_SIGNED, _current()),
    "standby_current": (0x1A, MILLI_SIGNED, _current()),
    "max_load_current": (0x1E, MILLI_SIGNED, _current()),
    CONF_AVERAGE_POWER: (
        0x24,
        MILLI_SIGNED,
        sensor.sensor_schema(
            kind=sensor.POWER, accuracy_decimals=3, entity_category=DIAG
        ),
    ),
    "temperature": (0x06, DECI_KELVIN, _temperature()),
    "internal_temperature": (0x28, DECI_KELVIN, _temperature()),
    "battery_level": (0x2C, RAW, sensor.sensor_schema(kind=sensor.BATTERY)),
    "state_of_health": (
        0x2E,
        RAW,
        sensor.sensor_schema(
            kind=sensor.PERCENT, icon=ICON_BATTERY, entity_category=DIAG
        ),
    ),
    "remaining_capacity": (0x10, RAW, _mah()),
    "full_charge_capacity": (0x12, RAW, _mah()),
    "design_capacity": (0x3C, RAW, _mah()),
    # Counts down while charging, so it goes below 0
    "raw_coulomb_count": (0x22, SIGNED, _mah()),
    "btp_discharge_set": (0x34, RAW, _mah()),
    "btp_charge_set": (0x36, RAW, _mah()),
    "time_to_empty": (0x16, SENTINEL, _minutes()),
    "time_to_full": (0x18, SENTINEL, _minutes()),
    "standby_time_to_empty": (0x1C, SENTINEL, _minutes()),
    "max_load_time_to_empty": (0x20, SENTINEL, _minutes()),
    "cycle_count": (
        0x2A,
        RAW,
        sensor.sensor_schema(
            accuracy_decimals=0,
            state_class=STATE_CLASS_TOTAL_INCREASING,
            icon=ICON_BATTERY,
            entity_category=DIAG,
        ),
    ),
    "charge_voltage": (0x30, MILLI_SENTINEL, _voltage()),
    "charge_current": (0x32, MILLI_SENTINEL, _current()),
    # CONTROL_STATUS()[BATT_ID2:0]
    "battery_id": (
        0x00,
        LOW3,
        sensor.sensor_schema(accuracy_decimals=0, entity_category=DIAG),
    ),
}

CONFIG_SCHEMA = cv.All(
    BQ27220_FULL_CHILD_SCHEMA.extend(
        {cv.Optional(key): schema for key, (_, _, schema) in SENSORS.items()}
    ),
    cv.has_at_least_one_key(*SENSORS),
)


async def to_code(config: ConfigType) -> None:
    hub_id = config[CONF_BQ27220_FULL_ID]
    hub = await cg.get_variable(hub_id)
    scale = fv.get_declaring_config(hub_id, CORE.config)[CONF_AVERAGE_POWER_SCALE]
    sensors = sensor.sub_sensors(config)
    for key, (reg, conversion, _) in SENSORS.items():
        if (var := await sensors(key, None)) is None:
            continue
        value = READING.value(HexIntLiteral(reg), conversion)
        if key == CONF_AVERAGE_POWER and scale != 1.0:
            value = value * scale
        STATUS_PUBLISHER.add(hub, parent_ref(var).publish_state(value))
        add_dump(hub, sensor.sensor_ns.log_sensor, key, var)

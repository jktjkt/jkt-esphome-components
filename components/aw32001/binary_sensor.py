from typing import Any

from esphome.automation import parent_ref
import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_BATTERY_CHARGING,
    DEVICE_CLASS_PLUG,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_RUNNING,
    ENTITY_CATEGORY_DIAGNOSTIC,
)
from esphome.cpp_generator import HexIntLiteral
from esphome.types import ConfigType

from . import AW32001_CHILD_SCHEMA, CONF_AW32001_ID, FAULT, STATUS, STATUS_PUBLISHER

DEPENDENCIES = ["aw32001"]

# REG09[5:0]; bits 7:6 are the shipping deglitch setting, not a fault
FAULT_MASK = 0x3F
STATUS_WATCHDOG = 0x80


def _bit(reg: cg.MockObj, bit: int) -> cg.MockObj:
    return (reg >> bit) & 1


def _charge_state(*states: int) -> cg.MockObj:
    """Whether CHG_STAT (REG08[4:3]: idle, pre-charge, fast charge, done) is one of states."""
    match = sum(1 << state for state in states)
    return (cg.MockObj(f"{match:#06b}") >> ((STATUS >> 3) & 0x03)) & 1


# key -> (device class, state from REG08 and REG09)
_SENSORS: dict[str, tuple[Any, cg.MockObj]] = {
    "power_good": (DEVICE_CLASS_PLUG, _bit(STATUS, 1)),
    "charging": (DEVICE_CLASS_BATTERY_CHARGING, _charge_state(1, 2)),
    "charge_done": (cv.UNDEFINED, _charge_state(3)),
    "power_path_regulation": (DEVICE_CLASS_RUNNING, _bit(STATUS, 2)),
    "thermal_regulation": (DEVICE_CLASS_RUNNING, _bit(STATUS, 0)),
    "watchdog_fault": (DEVICE_CLASS_PROBLEM, _bit(STATUS, 7)),
    "input_fault": (DEVICE_CLASS_PROBLEM, _bit(FAULT, 5)),
    "thermal_shutdown": (DEVICE_CLASS_PROBLEM, _bit(FAULT, 4)),
    "battery_ovp": (DEVICE_CLASS_PROBLEM, _bit(FAULT, 3)),
    "safety_timer_fault": (DEVICE_CLASS_PROBLEM, _bit(FAULT, 2)),
    "ntc_hot": (DEVICE_CLASS_PROBLEM, _bit(FAULT, 1)),
    "ntc_cold": (DEVICE_CLASS_PROBLEM, _bit(FAULT, 0)),
    "fault": (
        DEVICE_CLASS_PROBLEM,
        (
            (STATUS & HexIntLiteral(STATUS_WATCHDOG))
            | (FAULT & HexIntLiteral(FAULT_MASK))
        )
        != 0,
    ),
}

CONFIG_SCHEMA = cv.All(
    AW32001_CHILD_SCHEMA.extend(
        {
            cv.Optional(key): binary_sensor.binary_sensor_schema(
                device_class=device_class, entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            )
            for key, (device_class, _) in _SENSORS.items()
        }
    ),
    cv.has_at_least_one_key(*_SENSORS),
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_AW32001_ID])
    sensors = binary_sensor.sub_binary_sensors(config)
    for key, (_, state) in _SENSORS.items():
        if (var := await sensors(key, None)) is not None:
            STATUS_PUBLISHER.add(hub, parent_ref(var).publish_state(state))

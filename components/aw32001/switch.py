from esphome.automation import parent_ref
import esphome.codegen as cg
from esphome.components import switch
import esphome.config_validation as cv
from esphome.cpp_generator import HexIntLiteral
from esphome.types import ConfigType

from . import (
    AW32001_CHILD_SCHEMA,
    CONF_AW32001_ID,
    REG_MAIN,
    REG_POWER_ON,
    SWITCH_PUBLISHER,
)

DEPENDENCIES = ["aw32001"]

# key -> (register, mask, on when the bits are clear)
_SWITCHES = {
    "charge_enable": (REG_POWER_ON, 0x08, True),  # CEB, 0 = charge enabled
    "hiz": (REG_POWER_ON, 0x10, False),  # EN_HIZ
    "ship_mode": (
        REG_MAIN,
        0x20,
        False,
    ),  # FET_DIS, battery off after the deglitch delay
}
_REGISTERS = {
    REG_POWER_ON: SWITCH_PUBLISHER.param("power_on"),
    REG_MAIN: SWITCH_PUBLISHER.param("main_control"),
}

CONFIG_SCHEMA = cv.All(
    AW32001_CHILD_SCHEMA.extend(
        {
            cv.Optional(key): switch.switch_schema(
                switch.ApplySwitch, default_restore_mode="DISABLED"
            )
            for key in _SWITCHES
        }
    ),
    cv.has_at_least_one_key(*_SWITCHES),
)


async def to_code(config: ConfigType) -> None:
    hub = await cg.get_variable(config[CONF_AW32001_ID])
    switches = switch.sub_switches(config, parent=hub)
    for key, (reg, mask, inverted) in _SWITCHES.items():
        on, off = (0, mask) if inverted else (mask, 0)
        apply = f"update_bits({reg:#04x}, {mask:#04x}, state ? {on:#04x} : {off:#04x})"
        var = await switches(key, None, apply=apply, publish="on_success")
        if var is not None:
            bits = _REGISTERS[reg] & HexIntLiteral(mask)
            state = bits == 0 if inverted else bits != 0
            SWITCH_PUBLISHER.add(hub, parent_ref(var).publish_state(state))

from collections.abc import Callable

from esphome import automation
import esphome.codegen as cg
from esphome.components import i2c
from esphome.components.const import (
    CONF_CHARGE_ENABLED,
    CONF_INPUT_CURRENT_LIMIT,
    CONF_PRECHARGE_CURRENT,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL, CONF_WATCHDOG
from esphome.core import HexInt
from esphome.types import ConfigType

CODEOWNERS = []
DEPENDENCIES = ["i2c"]
DOMAIN = "aw32001"
MULTI_CONF = True

CONF_AW32001_ID = "aw32001_id"
CONF_FAST_CHARGE_CURRENT = "fast_charge_current"
CONF_TERMINATION_CURRENT = "termination_current"
CONF_CHARGE_VOLTAGE = "charge_voltage"
CONF_VINDPM = "vindpm"
CONF_CONFIG_ID = "config_id"

aw32001_ns = cg.esphome_ns.namespace("aw32001")
AW32001Component = aw32001_ns.class_(
    "AW32001Component", cg.PollingComponent, i2c.I2CDevice
)
AW32001RegisterUpdate = aw32001_ns.struct("AW32001RegisterUpdate")

AW32001_CHILD_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_AW32001_ID): cv.use_id(AW32001Component)}
)

# Every poll passes REG08 (status) and REG09 (faults) to the binary and text sensors
STATUS_PUBLISHER = cg.HubFunction(
    "status",
    cg.void,
    [(cg.uint8, "status"), (cg.uint8, "fault")],
    define="USE_AW32001_STATUS_PUBLISHER",
)
STATUS = STATUS_PUBLISHER.param("status")
FAULT = STATUS_PUBLISHER.param("fault")
# REG01 and REG06, read only when the hub has switches
SWITCH_PUBLISHER = cg.HubFunction(
    "switches",
    cg.void,
    [(cg.uint8, "power_on"), (cg.uint8, "main_control")],
    define="USE_AW32001_SWITCH_PUBLISHER",
)

# Register map: AW32001E datasheet V1.2 "Register List" (see docs/aw32001.md)
REG_INPUT = 0x00
REG_POWER_ON = 0x01
REG_ICHG = 0x02
REG_TERM = 0x03
REG_VBAT = 0x04
REG_TIMER = 0x05
REG_MAIN = 0x06
REG_INDIV = 0x0B
REG_EXTRA = 0x0C
# REG02[7:6] are the reset of all registers and the self-clearing watchdog reset: always written as 0
ICHG_RESET_BITS = 0xC0

DEFAULT_CHARGE_VOLTAGE = 4.2
VINDPM_HEADROOM_MV = 250

WATCHDOG_OPTIONS = {"disabled": 0, "40s": 1, "80s": 2, "160s": 3}
WATCHDOG_PERIOD_S = {"40s": 40, "80s": 80, "160s": 160}

# IIN_LIM code -> mA (REG00[3:0]); 550 mA is code 15 plus REG0C[7] (EN0P55)
IIN_LIM_MA = [
    50,
    80,
    110,
    140,
    170,
    200,
    230,
    260,
    290,
    320,
    350,
    380,
    410,
    440,
    470,
    500,
]


def _milli(value: float) -> int:
    """mA from A, or mV from V."""
    return round(value * 1000)


def _fast_charge(value: float) -> float:
    ma = _milli(value)
    if not 8 <= ma <= 456 or ma % 8:
        raise cv.Invalid(
            f"fast_charge_current must be a multiple of 8 mA from 8 mA to 456 mA, got {ma} mA"
        )
    return value


def _odd_ma(name: str) -> Callable[[float], float]:
    def validator(value: float) -> float:
        ma = _milli(value)
        if not 1 <= ma <= 31 or ma % 2 == 0:
            raise cv.Invalid(
                f"{name} must be an odd number of mA from 1 mA to 31 mA (1 + 2 mA steps), got {ma} mA"
            )
        return value

    return validator


def _charge_voltage(value: float) -> float:
    mv = _milli(value)
    if not 3600 <= mv <= 4200 or (mv - 3600) % 15:
        raise cv.Invalid(
            f"charge_voltage must be 3.6 V + n * 15 mV, up to 4.2 V (the Nesso N1 cell is a 4.2 V LiPo), got {mv} mV"
        )
    return value


def _input_current(value: float) -> float:
    ma = _milli(value)
    if ma not in IIN_LIM_MA and ma != 550:
        raise cv.Invalid(
            f"input_current_limit must be one of {[*IIN_LIM_MA, 550]} mA, got {ma} mA"
        )
    return value


def _vindpm(value: float) -> float:
    mv = _milli(value)
    if not 3880 <= mv <= 5080 or (mv - 3880) % 80:
        raise cv.Invalid(
            f"vindpm must be 3.88 V + n * 80 mV, from 3.88 V to 5.08 V, got {mv} mV"
        )
    return value


def _check_watchdog(config: ConfigType) -> ConfigType:
    period = WATCHDOG_PERIOD_S.get(config[CONF_WATCHDOG])
    interval = config[CONF_UPDATE_INTERVAL]
    # One missed poll must not let the watchdog expire, so at least two polls fit in a period
    if period is not None and interval.total_milliseconds * 2 > period * 1000:
        raise cv.Invalid(
            f"update_interval must be at most half of the {period} s watchdog ({period // 2} s): the poll "
            "kicks the watchdog, otherwise the charger cuts the system power and falls back to its defaults. "
            "Use a shorter update_interval or watchdog: disabled",
            path=[CONF_UPDATE_INTERVAL],
        )
    return config


def _check_vindpm(config: ConfigType) -> ConfigType:
    # Datasheet: VIN_DPM must be at least 250 mV above VBAT_REG for a stable regulator.
    # An unset vindpm keeps the chip default of 4.52 V, which is enough for the 4.2 V maximum.
    if (vindpm := config.get(CONF_VINDPM)) is None:
        return config
    vbat_mv = _milli(config.get(CONF_CHARGE_VOLTAGE, DEFAULT_CHARGE_VOLTAGE))
    if (vindpm_mv := _milli(vindpm)) < vbat_mv + VINDPM_HEADROOM_MV:
        raise cv.Invalid(
            f"vindpm must be at least {VINDPM_HEADROOM_MV} mV above the charge voltage "
            f"({vbat_mv} mV), got {vindpm_mv} mV",
            path=[CONF_VINDPM],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AW32001Component),
            cv.GenerateID(CONF_CONFIG_ID): cv.declare_id(AW32001RegisterUpdate),
            cv.Optional(CONF_CHARGE_ENABLED): cv.boolean,
            cv.Optional(CONF_FAST_CHARGE_CURRENT): cv.All(cv.current, _fast_charge),
            cv.Optional(CONF_TERMINATION_CURRENT): cv.All(
                cv.current, _odd_ma(CONF_TERMINATION_CURRENT)
            ),
            cv.Optional(CONF_PRECHARGE_CURRENT): cv.All(
                cv.current, _odd_ma(CONF_PRECHARGE_CURRENT)
            ),
            cv.Optional(CONF_CHARGE_VOLTAGE): cv.All(cv.voltage, _charge_voltage),
            cv.Optional(CONF_INPUT_CURRENT_LIMIT): cv.All(cv.current, _input_current),
            cv.Optional(CONF_VINDPM): cv.All(cv.voltage, _vindpm),
            cv.Optional(CONF_WATCHDOG, default="disabled"): cv.one_of(
                *WATCHDOG_OPTIONS, lower=True
            ),
        }
    )
    .extend(cv.polling_component_schema("30s"))
    .extend(i2c.i2c_device_schema(0x49)),
    _check_watchdog,
    _check_vindpm,
)


def register_updates(config: ConfigType) -> list[tuple[int, int, int]]:
    """The (register, mask, value) updates the configuration asks for, one per register, in write order."""
    updates: dict[int, tuple[int, int]] = {}

    def put(reg: int, mask: int, value: int) -> None:
        old_mask, old_value = updates.get(reg, (0, 0))
        updates[reg] = (old_mask | mask, old_value | value)

    if (current := config.get(CONF_FAST_CHARGE_CURRENT)) is not None:
        put(REG_ICHG, 0x3F | ICHG_RESET_BITS, _milli(current) // 8 - 1)
    if (current := config.get(CONF_TERMINATION_CURRENT)) is not None:
        put(REG_TERM, 0x0F, (_milli(current) - 1) // 2)
    if (current := config.get(CONF_PRECHARGE_CURRENT)) is not None:
        # EN_IPRE_SET (bit 5) selects REG0B[4:1] instead of the termination code
        put(REG_INDIV, 0x3E, 0x20 | ((_milli(current) - 1) // 2) << 1)
    if (voltage := config.get(CONF_CHARGE_VOLTAGE)) is not None:
        put(REG_VBAT, 0xFC, ((_milli(voltage) - 3600) // 15) << 2)
    if (current := config.get(CONF_INPUT_CURRENT_LIMIT)) is not None:
        if (ma := _milli(current)) == 550:
            put(REG_INPUT, 0x0F, 15)
            put(REG_EXTRA, 0x80, 0x80)
        else:
            put(REG_INPUT, 0x0F, IIN_LIM_MA.index(ma))
            put(REG_EXTRA, 0x80, 0)
    if (voltage := config.get(CONF_VINDPM)) is not None:
        put(REG_INPUT, 0xF0, ((_milli(voltage) - 3880) // 80) << 4)
    put(REG_TIMER, 0x60, WATCHDOG_OPTIONS[config[CONF_WATCHDOG]] << 5)
    if (enabled := config.get(CONF_CHARGE_ENABLED)) is not None:
        # Last, so charging never starts with the chip defaults. CEB: 0 = charge enabled
        put(REG_POWER_ON, 0x08, 0 if enabled else 0x08)
    return [(reg, mask, value) for reg, (mask, value) in updates.items()]


async def to_code(config: ConfigType) -> None:
    updates = register_updates(config)
    table = cg.progmem_array(
        config[CONF_CONFIG_ID],
        cg.ArrayInitializer(
            *(cg.ArrayInitializer(*map(HexInt, update)) for update in updates),
            multiline=True,
        ),
    )
    await i2c.new_i2c_component(
        config, table, len(updates), config[CONF_WATCHDOG] != "disabled"
    )


automation.register_apply_action(
    "aw32001.kick_watchdog",
    automation.maybe_simple_id({cv.Required(CONF_ID): cv.use_id(AW32001Component)}),
    automation.ApplyCall("kick_watchdog()"),
)

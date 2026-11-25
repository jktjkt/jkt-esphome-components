from esphome import automation
from esphome.automation import parent_ref
import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL
from esphome.types import ConfigType

CODEOWNERS = []
DEPENDENCIES = ["i2c"]
DOMAIN = "bq27220_full"
MULTI_CONF = True

CONF_BQ27220_FULL_ID = "bq27220_full_id"
CONF_DESIGN_CAPACITY = "design_capacity"
CONF_RESET_ON_BOOT = "reset_on_boot"
CONF_AVERAGE_POWER_SCALE = "average_power_scale"
CONF_TAPER_CURRENT = "taper_current"

bq27220_full_ns = cg.esphome_ns.namespace("bq27220_full")
BQ27220FullComponent = bq27220_full_ns.class_(
    "BQ27220FullComponent", cg.PollingComponent, i2c.I2CDevice
)
Reading = bq27220_full_ns.struct("Reading")
StatusWord = bq27220_full_ns.enum("StatusWord", is_class=True)

BQ27220_FULL_CHILD_SCHEMA = cv.Schema(
    {cv.GenerateID(CONF_BQ27220_FULL_ID): cv.use_id(BQ27220FullComponent)}
)

# What every poll read, decoded by the entities
STATUS_PUBLISHER = cg.HubFunction(
    "status",
    cg.void,
    [(Reading.operator("const").operator("ref"), "reading")],
    define="USE_BQ27220_FULL_STATUS_PUBLISHER",
)
READING = STATUS_PUBLISHER.param("reading")
# The dump lines of the entities above
DUMP = cg.HubFunction(
    "dump",
    cg.void,
    [(cg.const_char_ptr, "tag")],
    define="USE_BQ27220_FULL_STATUS_PUBLISHER",
)
DUMP_TAG = DUMP.param("tag")


def add_dump(hub: cg.MockObj, log: cg.MockObj, key: str, var: cg.MockObj) -> None:
    """Log the entity of ``key`` in the hub's dump_config(), with ``log`` such as ``sensor_ns.log_sensor``."""
    name = cg.RawExpression(f'LOG_STR_LITERAL("{key.replace("_", " ").title()}")')
    DUMP.add(hub, log(DUMP_TAG, "  ", name, parent_ref(var)))


# Everything below follows SLUUBD4A (BQ27220 Technical Reference Manual), chapter 2.

WORD_CONTROL = StatusWord.STATUS_WORD_CONTROL  # CONTROL_STATUS(), 0x00
WORD_BATTERY = StatusWord.STATUS_WORD_BATTERY  # BatteryStatus(), 0x0A
WORD_OPERATION = StatusWord.STATUS_WORD_OPERATION  # OperationStatus(), 0x3A
WORD_GAUGING = StatusWord.STATUS_WORD_GAUGING  # GaugingStatus() MAC subcommand 0x0056

# (word, bit, name) of every defined flag. Reserved bits are left out. The GaugingStatus
# flags TC, TD, FC and FD are documented as identical to BatteryStatus TCA, TDA, FC and
# FD, and its VDQ and EDV2 are copies of the OperationStatus ones, so they are not repeated.
FLAGS = (
    (WORD_CONTROL, 3, "SNOOZE"),
    (WORD_CONTROL, 4, "BCA"),
    (WORD_CONTROL, 5, "CCA"),
    (WORD_BATTERY, 0, "DSG"),
    (WORD_BATTERY, 1, "SYSDWN"),
    (WORD_BATTERY, 2, "TDA"),
    (WORD_BATTERY, 3, "BATTPRES"),
    (WORD_BATTERY, 4, "AUTH_GD"),
    (WORD_BATTERY, 5, "OCVGD"),
    (WORD_BATTERY, 6, "TCA"),
    (WORD_BATTERY, 8, "CHGINH"),
    (WORD_BATTERY, 9, "FC"),
    (WORD_BATTERY, 10, "OTD"),
    (WORD_BATTERY, 11, "OTC"),
    (WORD_BATTERY, 12, "SLEEP"),
    (WORD_BATTERY, 13, "OCVFAIL"),
    (WORD_BATTERY, 14, "OCVCOMP"),
    (WORD_BATTERY, 15, "FD"),
    (WORD_OPERATION, 0, "CALMD"),
    (WORD_OPERATION, 3, "EDV2"),
    (WORD_OPERATION, 4, "VDQ"),
    (WORD_OPERATION, 5, "INITCOMP"),
    (WORD_OPERATION, 6, "SMTH"),
    (WORD_OPERATION, 7, "BTPINT"),
    (WORD_OPERATION, 10, "CFGUPDATE"),
    (WORD_GAUGING, 6, "GS_DSG"),
    (WORD_GAUGING, 7, "CF"),
    (WORD_GAUGING, 5, "EDV"),
    (WORD_GAUGING, 13, "EDV1"),
    (WORD_GAUGING, 10, "FCCX"),
)


def _check_update_interval(config: ConfigType) -> ConfigType:
    # SLUUBD4A 5.3: more than two sweeps of the standard commands per second can reset the gauge
    if config[CONF_UPDATE_INTERVAL].total_milliseconds < 500:
        raise cv.Invalid(
            "update_interval must be at least 500 ms: the gauge can reset itself "
            "when the standard commands are read more than twice per second",
            path=[CONF_UPDATE_INTERVAL],
        )
    return config


def _design_capacity(value: object) -> int:
    mah = round(cv.float_with_unit("capacity", "Ah")(value) * 1000)
    if not 1 <= mah <= 32767:
        raise cv.Invalid(f"design_capacity must be 1 to 32767 mAh, got {mah} mAh")
    return mah


def _taper_current(value: object) -> int:
    ma = round(cv.current(value) * 1000)
    if not 1 <= ma <= 1000:
        raise cv.Invalid(f"taper_current must be 1 to 1000 mA, got {ma} mA")
    return ma


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BQ27220FullComponent),
            cv.Optional(CONF_DESIGN_CAPACITY): _design_capacity,
            cv.Optional(CONF_TAPER_CURRENT): _taper_current,
            cv.Optional(CONF_RESET_ON_BOOT, default=False): cv.boolean,
            cv.Optional(CONF_AVERAGE_POWER_SCALE, default=1.0): cv.positive_float,
        }
    )
    .extend(cv.polling_component_schema("10s"))
    .extend(i2c.i2c_device_schema(0x55)),
    _check_update_interval,
)


async def to_code(config: ConfigType) -> None:
    var = await i2c.new_i2c_component(config)
    cg.add_optional(var.set_design_capacity, config.get(CONF_DESIGN_CAPACITY))
    cg.add_optional(var.set_taper_current, config.get(CONF_TAPER_CURRENT))
    if config[CONF_RESET_ON_BOOT]:
        cg.add(var.set_reset_on_boot(True))
    # average_power_scale is applied by the average_power sensor


automation.register_apply_action(
    "bq27220_full.reset",
    automation.maybe_simple_id({cv.Required(CONF_ID): cv.use_id(BQ27220FullComponent)}),
    automation.ApplyCall("reset_gauge()"),
)

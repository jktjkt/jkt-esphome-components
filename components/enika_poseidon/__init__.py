from typing import Any

from esphome import automation
import esphome.codegen as cg
from esphome.components import sx126x
from esphome.components.const import CONF_ON_PACKET
import esphome.config_validation as cv
from esphome.const import CONF_ID
import esphome.final_validate as fv
from esphome.types import ConfigType

CODEOWNERS = []
DEPENDENCIES = ["sx126x"]
DOMAIN = "enika_poseidon"

CONF_RADIO = "radio"
CONF_ON_STATE_MESSAGE = "on_state_message"

enika_poseidon_ns = cg.esphome_ns.namespace("enika_poseidon")
PoseidonReceiver = enika_poseidon_ns.class_(
    "PoseidonReceiver", cg.Component, sx126x.SX126xListener
)
StateMessage = enika_poseidon_ns.struct("StateMessage")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PoseidonReceiver),
        cv.Required(CONF_RADIO): cv.use_id(sx126x.SX126x),
        cv.Optional(CONF_ON_PACKET): automation.validate_automation({}),
        cv.Optional(CONF_ON_STATE_MESSAGE): automation.validate_automation({}),
    }
).extend(cv.COMPONENT_SCHEMA)

# What the packet framing needs from the radio. The frequency, deviation and RX bandwidth are not checked: a weak
# transmitter may want other values than the 868.3 MHz, 20 kHz and 117.3 kHz of the example board.
_RADIO_SETTINGS: tuple[tuple[str, Any, str], ...] = (
    ("modulation", "FSK", "FSK"),
    ("bitrate", 38400, "38400"),
    ("sync_value", [0xCE, 0x3C], "[0xCE, 0x3C]"),
    ("crc_enable", True, "true"),
    ("crc_size", 2, "2"),
    ("crc_polynomial", 0x8005, "0x8005"),
    ("crc_initial", 0xFFFF, "0xFFFF"),
    ("crc_inverted", False, "false"),
    ("whitening_enable", False, "false"),
    ("payload_length", 0, "0 (variable length)"),
)


def _final_validate(config: ConfigType) -> ConfigType:
    full = fv.full_config.get()
    path = full.get_path_for_id(config[CONF_RADIO])[:-1]
    radio = full.get_config_for_path(path)
    for key, expected, text in _RADIO_SETTINGS:
        actual = radio[key]
        if (str(actual) if isinstance(expected, str) else actual) != expected:
            raise cv.Invalid(
                f"The Poseidon packet format needs {key}: {text} on the sx126x radio, got {actual}",
                path=[*path, key],
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate

_PACKET_ARGS = [
    (cg.byte_span, "frame"),
    (cg.float_, "rssi"),
    (cg.StringRef, "summary"),
]
# An automation that defers keeps its arguments, so it gets copies instead of views into the receive buffer
_frame = cg.MockObj("frame")
_rssi = cg.MockObj("rssi")
_summary = cg.MockObj("summary")
_OWNING_PACKET_ARGS = [
    (cg.std_vector.template(cg.uint8), "frame"),
    (cg.float_, "rssi"),
    (cg.std_string, "summary"),
]
_OWNING_PACKET_FORWARD = [
    cg.std_vector.template(cg.uint8)(_frame.begin(), _frame.end()),
    _rssi,
    _summary.str(),
]


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    radio = await cg.get_variable(config[CONF_RADIO])
    cg.add(radio.register_listener(var))
    for conf in config.get(CONF_ON_PACKET, []):
        if automation.has_non_synchronous_actions(conf):
            await automation.build_callback_automation(
                var,
                "add_on_packet_callback",
                _OWNING_PACKET_ARGS,
                conf,
                params=_PACKET_ARGS,
                forward=_OWNING_PACKET_FORWARD,
            )
        else:
            await automation.build_callback_automation(
                var, "add_on_packet_callback", _PACKET_ARGS, conf
            )
    for conf in config.get(CONF_ON_STATE_MESSAGE, []):
        await automation.build_callback_automation(
            var,
            "add_on_state_message_callback",
            [(StateMessage, "message"), (cg.float_, "rssi")],
            conf,
        )

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import remote_base
from esphome.components.remote_base import CONF_RECEIVER_ID
import esphome.config_validation as cv
from esphome.const import (
    CONF_ENABLE_PIN,
    CONF_FILTER,
    CONF_ID,
    CONF_IDLE,
    CONF_INVERTED,
    CONF_PIN,
)
import esphome.final_validate as fv
from esphome.types import ConfigType

CODEOWNERS = []
DEPENDENCIES = ["remote_receiver"]
DOMAIN = "scs_sniffer"

CONF_ON_FRAME = "on_frame"

scs_sniffer_ns = cg.esphome_ns.namespace("scs_sniffer")
SCSSniffer = scs_sniffer_ns.class_(
    "SCSSniffer", cg.Component, remote_base.RemoteReceiverListener
)
Frame = scs_sniffer_ns.struct("Frame")
Message = scs_sniffer_ns.struct("Message")

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SCSSniffer),
            cv.Optional(CONF_ENABLE_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_ON_FRAME): automation.validate_automation({}),
        }
    )
    .extend(remote_base.REMOTE_LISTENER_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config: ConfigType) -> ConfigType:
    # The pulses of a healthy tap are 30-34 us long and the decoder drops anything shorter than 8 us itself, so the
    # receiver's filter must not eat them.
    full = fv.full_config.get()
    path = full.get_path_for_id(config[CONF_RECEIVER_ID])[:-1]
    receiver = fv.get_declaring_config(config[CONF_RECEIVER_ID])

    if not receiver[CONF_PIN].get(CONF_INVERTED, False):
        raise cv.Invalid(
            "The tap's output is active low and the decoder wants the pulses as marks: set 'inverted: true' on the "
            "pin of the remote_receiver",
            path=[*path, CONF_PIN],
        )
    if receiver[CONF_FILTER].total_microseconds >= 20:
        raise cv.Invalid(
            "The SCS pulses are about 34 us long, a filter of 20 us or more drops them: use 'filter: 2us'",
            path=[*path, CONF_FILTER],
        )
    if receiver[CONF_IDLE].total_microseconds < 1000:
        raise cv.Invalid(
            "The gaps inside an SCS frame are up to 0.8 ms, a shorter 'idle' splits the frames: use 'idle: 2ms'",
            path=[*path, CONF_IDLE],
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await remote_base.register_listener(var, config)
    await cg.add_pin(var.set_enable_pin, config.get(CONF_ENABLE_PIN))
    # The summary is a view into a buffer that lives during the callback only
    args = [(Frame, "frame"), (Message, "message"), (cg.StringRef, "summary")]
    for conf in config.get(CONF_ON_FRAME, []):
        await automation.build_callback_automation(
            var,
            "add_on_frame_callback",
            automation.owning_args(args, conf),
            conf,
        )

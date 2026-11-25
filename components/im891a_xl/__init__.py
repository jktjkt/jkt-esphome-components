import struct

from esphome import automation, pins
import esphome.codegen as cg
from esphome.components import uart
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_FILE,
    CONF_NAME,
    CONF_RAW_DATA_ID,
    CONF_RESET_PIN,
)
from esphome.core import HexInt, TimePeriod
from esphome.types import ConfigType

CODEOWNERS = []
DEPENDENCIES = ["uart"]
DOMAIN = "im891a_xl"

CONF_LINK_MODE = "link_mode"
CONF_RECALIBRATION_TIMEOUT = "recalibration_timeout"
CONF_HEALTH_CHECK_INTERVAL = "health_check_interval"
CONF_ON_PACKET = "on_packet"
CONF_ON_IMAGE_LINE = "on_image_line"
CONF_IMAGE_PARITY = "image_parity"
CONF_IMAGE_DETECT_RESTART = "image_detect_restart"
CONF_RAM_IMAGES = "ram_images"
CONF_VERIFY = "verify"
CONF_VIA_HCI = "via_hci"
CONF_IMAGE = "image"
CONF_RAM_IMAGES_ID = "ram_images_id"

im891a_xl_ns = cg.esphome_ns.namespace("im891a_xl")
IM891AXL = im891a_xl_ns.class_("IM891AXL", cg.Component, uart.UARTDevice)
WMBusPacket = im891a_xl_ns.namespace("hci").struct("WMBusPacket")
RamImage = im891a_xl_ns.struct("RamImage")

# Link modes of the WM-Bus gateway firmware, as numbered in its Get/Set Active Configuration messages
LINK_MODES = {
    "S": 1,
    "T": 2,
    "C_T": 3,  # T and C mode at the same time
    "C": 5,  # 100 kbit/s
    "ENHANCED_T": 6,
    "CUSTOM_C_T": 7,  # for some Engelmann meters
}

# The RAM of the STM32WLE5 is 0x20000000 to 0x2000FFFF. The ROM bootloader uses the first half, and IMST's firmware
# keeps a request flag for the bootloader at 0x2000FFF0, so an image goes between 0x20008000 and that flag.
RAM_IMAGE_START = 0x20008000
RAM_IMAGE_END = 0x2000FFF0


def _ram_image(config: ConfigType) -> ConfigType:
    data = config[CONF_FILE].read_bytes()
    address = config[CONF_ADDRESS]
    if len(data) < 8:
        raise cv.Invalid(
            f"{config[CONF_FILE]} is too short for an image", path=[CONF_FILE]
        )
    if address % 4:
        raise cv.Invalid("The address must be a multiple of 4", path=[CONF_ADDRESS])
    if address < RAM_IMAGE_START or address + len(data) > RAM_IMAGE_END:
        raise cv.Invalid(
            f"The image would occupy 0x{address:08X} to 0x{address + len(data):08X}, "
            f"it has to be within 0x{RAM_IMAGE_START:08X} to 0x{RAM_IMAGE_END:08X}: the ROM bootloader "
            "uses the RAM below, and the module keeps a flag above",
            path=[CONF_ADDRESS],
        )
    # The bootloader starts a program by taking the stack pointer from its first word and jumping to its second
    stack, reset = struct.unpack_from("<II", data)
    if not address <= (reset & ~1) < address + len(data):
        raise cv.Invalid(
            f"The reset vector of the image, 0x{reset:08X}, is not inside the image at 0x{address:08X}: "
            "is it built for the RAM, and for this address?",
            path=[CONF_FILE],
        )
    return config


RAM_IMAGE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_RAW_DATA_ID): cv.declare_id(cg.uint8),
            cv.Required(CONF_NAME): cv.string_strict,
            cv.Required(CONF_FILE): cv.file_,
            cv.Optional(CONF_ADDRESS, default=RAM_IMAGE_START): cv.hex_uint32_t,
            cv.Optional(CONF_VERIFY, default=True): cv.boolean,
        }
    ),
    _ram_image,
)


def _unique_image_names(images: list[ConfigType]) -> list[ConfigType]:
    names = [image[CONF_NAME] for image in images]
    for name in names:
        if names.count(name) > 1:
            raise cv.Invalid(f"There are several images named {name}")
    return images


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(IM891AXL),
            cv.Optional(CONF_RESET_PIN): pins.gpio_output_pin_schema,
            cv.Optional(CONF_LINK_MODE, default="C_T"): cv.one_of(
                *LINK_MODES, upper=True
            ),
            # the longest the receiver listens before the radio is calibrated again, the module wants at least 100 ms
            cv.Optional(CONF_RECALIBRATION_TIMEOUT, default="10min"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=TimePeriod(milliseconds=100)),
            ),
            cv.Optional(CONF_HEALTH_CHECK_INTERVAL, default="30s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=TimePeriod(seconds=1)),
            ),
            cv.Optional(CONF_ON_PACKET): automation.validate_automation({}),
            # Images that can be loaded into the RAM of the module and started, see the actions below
            cv.Optional(CONF_RAM_IMAGES): cv.All(
                cv.ensure_list(RAM_IMAGE_SCHEMA), _unique_image_names
            ),
            # the table of the images, a static array: nothing is allocated for it at run time
            cv.GenerateID(CONF_RAM_IMAGES_ID): cv.declare_id(RamImage),
            # The UART format of a started image, the module's firmware speaks 8N1 and the bootloader 8E1
            cv.Optional(CONF_IMAGE_PARITY, default="NONE"): cv.enum(
                uart.UART_PARITY_OPTIONS, upper=True
            ),
            # Go back to the module's firmware when a valid HCI frame comes out of the image's UART, see the docs
            cv.Optional(CONF_IMAGE_DETECT_RESTART, default=True): cv.boolean,
            cv.Optional(CONF_ON_IMAGE_LINE): automation.validate_automation({}),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    DOMAIN,
    baud_rate=115200,
    require_tx=True,
    require_rx=True,
    data_bits=8,
    parity="NONE",
    stop_bits=1,
)


async def to_code(config: ConfigType) -> None:
    var = await uart.new_uart_component(config)
    await cg.add_pin(var.set_reset_pin, config.get(CONF_RESET_PIN))
    cg.add(var.set_link_mode(LINK_MODES[config[CONF_LINK_MODE]]))
    cg.add(
        var.set_recalibration_timeout(
            config[CONF_RECALIBRATION_TIMEOUT].total_milliseconds
        )
    )
    cg.add(
        var.set_health_check_interval(
            config[CONF_HEALTH_CHECK_INTERVAL].total_milliseconds
        )
    )
    cg.add(var.set_image_parity(config[CONF_IMAGE_PARITY]))
    cg.add(var.set_image_detect_restart(config[CONF_IMAGE_DETECT_RESTART]))
    if images := config.get(CONF_RAM_IMAGES):
        entries = []
        for image in images:
            data = image[CONF_FILE].read_bytes()
            raw = cg.progmem_array(image[CONF_RAW_DATA_ID], [HexInt(b) for b in data])
            entries.append(
                cg.StructInitializer(
                    RamImage,
                    ("name", image[CONF_NAME]),
                    ("data", raw),
                    ("size", len(data)),
                    ("address", HexInt(image[CONF_ADDRESS])),
                    ("verify", image[CONF_VERIFY]),
                )
            )
        table = cg.static_const_array(
            config[CONF_RAM_IMAGES_ID], cg.ArrayInitializer(*entries, multiline=True)
        )
        cg.add(var.set_ram_images(table, len(entries)))
    for conf in config.get(CONF_ON_PACKET, []):
        await automation.build_callback_automation(
            var,
            "add_on_packet_callback",
            [(WMBusPacket.operator("ref").operator("const"), "packet")],
            conf,
        )
    # The line lives only while the callback runs, an automation that defers gets a copy
    for conf in config.get(CONF_ON_IMAGE_LINE, []):
        await automation.build_callback_automation(
            var,
            "add_on_image_line_callback",
            automation.owning_args([(cg.StringRef, "line")], conf),
            conf,
        )


# Actions. The module changes the UART format while they run, see the driver's documentation.
NO_ARGS_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(IM891AXL)},
)

START_BOOTLOADER_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(IM891AXL),
        # false: the module is in the bootloader already, e.g. BOOT0 was held at its reset
        cv.Optional(CONF_VIA_HCI, default=True): cv.boolean,
    }
)

LOAD_IMAGE_SCHEMA = cv.maybe_simple_value(
    {
        cv.GenerateID(): cv.use_id(IM891AXL),
        cv.Required(CONF_IMAGE): cv.templatable(cv.string),
    },
    key=CONF_IMAGE,
)

automation.register_apply_action(
    "im891a_xl.start_bootloader",
    START_BOOTLOADER_SCHEMA,
    automation.ApplyCall("start_bootloader({})", ((CONF_VIA_HCI, cg.bool_),)),
)
automation.register_apply_action(
    "im891a_xl.load_image",
    LOAD_IMAGE_SCHEMA,
    automation.ApplyField(CONF_IMAGE, "load_image", cg.std_string),
)
automation.register_apply_action(
    "im891a_xl.reset",
    NO_ARGS_SCHEMA,
    automation.ApplyCall("reset()"),
)

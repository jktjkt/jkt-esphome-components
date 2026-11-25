# im891a_xl

ESPHome external component for the IMST iM891A-XL radio module running the WM-Bus Gateway firmware, over its UART.
It sets the module up for receiving, keeps it configured, and gives every received Wireless M-Bus packet to an automation.
The protocol is the WiMOD HCI of the "Wireless M-Bus Gateway Firmware Host Controller Interface Protocol" specification, version 2.4.

Only the WM-Bus receive mode is supported for now, plus restarting the module into its ROM bootloader and loading a program into its RAM, see below.
Not implemented: sending, the packet scan mode, the device list with its keys (the module can filter and decrypt), setting the module's clock, the gateway status report, and the system options.
The iM881A-XL and the iU891A-XL run the same firmware and protocol, but this driver warns about every module that is not an iM891A-XL, and was not tried with them.

## What it does

- Resets the module through the optional reset pin, and asks it for a sign of life (ping), its device information and its firmware information.
- Sets the active configuration: the link mode, packet notifications on, tx notifications off, radio re-calibration on, LEDs off.
  The active configuration lives in the RAM of the module, the non-volatile default configuration is never written.
- Reads the configuration back, and only then reports the module as ready.
- Checks every `health_check_interval` that the module still has the configuration, and sets it again if not.
  A module that restarts on its own forgets it, and announces the restart only if its system options say so.
- Starts over, after a pause of 5 seconds, if the module does not answer a request three times, or refuses it.
- Delivers each received packet to `on_packet`.
  At the `VERBOSE` log level it also logs each packet: RSSI, link mode, manufacturer and device ID, and the bytes.
  To see the packets at the default `DEBUG` level, log them from `on_packet`, as in the example below.
- Allocates nothing on the heap while it runs.
  Frames, the lines of a RAM image and the bootloader's state live in buffers of a fixed size inside the component, about 1.7 kB, and the table of RAM images is a static array.
  Only what an automation asks for goes to the heap: an automation with `delay:` or another action that waits keeps a copy of its `packet` or `line`.

## Configuration

```yaml
uart:
  id: imst_uart
  tx_pin: GPIO4
  rx_pin: GPIO5
  baud_rate: 115200
  rx_buffer_size: 1024

im891a_xl:
  id: imst
  uart_id: imst_uart
  reset_pin: GPIO6       # to nRESET of the module, which is active low
  link_mode: C_T         # default
  recalibration_timeout: 10min   # default
  health_check_interval: 30s     # default
  ram_images:                    # optional, see "Bootloader and RAM images"
    - name: sniffer
      file: poseidon-sniffer-ram.bin
  on_packet:
    - logger.log:
        format: "%s %d dBm"
        args: [packet.meter_id().data(), packet.rssi]
```


| option | default | meaning |
|---|---|---|
| `uart_id` | | the UART of the module, 115200 baud, 8N1 (checked) |
| `reset_pin` | none | hardware reset, an output. The driver holds it high and pulls it low for 10 ms to reset the module, so it goes straight to nRESET; set `inverted` only if something inverts it on the way. Without it the driver can only ask the module again, and a hardware reset is the way out of a module that does not answer. |
| `link_mode` | `C_T` | `S`, `T`, `C_T` (T and C at once), `C` (100 kbit/s), `ENHANCED_T`, or `CUSTOM_C_T` (for some Engelmann meters, firmware 2.4) |
| `recalibration_timeout` | `10min` | the longest the receiver listens before it calibrates the radio again, at least 100 ms |
| `health_check_interval` | `30s` | how often the module's configuration is read back, at least 1 s |
| `on_packet` | none | automation for every received packet, with `packet` (a `const im891a_xl::hci::WMBusPacket &`) |
| `ram_images` | none | programs for the RAM of the module, see below |
| `image_parity` | `NONE` | UART parity of a program that was started: `NONE`, `EVEN` or `ODD` |
| `image_detect_restart` | `true` | go back to the module's firmware when a valid HCI frame comes out of the UART of a started program, see below |
| `on_image_line` | none | automation for every line a started program prints, with `line`: a `StringRef` of up to 512 characters (a `std::string` in an automation that waits), with `c_str()`, `size()` and `str()` |

A frame with its CRC wrong is dropped by the module, and is not counted here.
`rx_buffer_size: 1024` is advised, since a frame is up to about 510 bytes on the wire and `loop()` runs between them.

## packet

Every field of the indication is in some member of `hci::WMBusPacket`.


| member | meaning |
|---|---|
| `timestamp` | UTC seconds of the module's clock. Not valid unless the clock was set, which this driver does not do. |
| `decryption_status` | 0 not encrypted, 1 decrypted, 2 no key, 3 mode not supported, 4 decryption error. Always 0 or 2 here, as no device list is loaded. |
| `encryption_mode` | 0 none, 5 mode 5, 7 mode 7, 128 ELL AES counter, 129 custom, 255 not supported |
| `packet_info` | link mode and frame format of the received packet, as numbered by the firmware: 1 S, 2 T, 4 and 5 C at 50 and 100 kbit/s format A, 6 enhanced T, 7 custom T, 8 custom C format A, 20 and 21 C format B, 24 custom C format B. `LOG_STR_ARG(im891a_xl::packet_info_to_str(packet.packet_info))` has it as text. |
| `rssi` | dBm |
| `data()` | the WM-Bus packet as a `std::span<const uint8_t>`: L-field, C-field, manufacturer, device ID, version, device type, and the rest. Up to 492 bytes. |

`data()` has the physical layer CRCs removed, and the L-field of a format B packet is corrected for them, so it is the same for every frame format.
It begins at the L-field, as `wmbus_radio::Frame::data()` does.
The span has `size()`, `data()`, `[]`, `begin()` and `end()`.
Where a function wants a pointer, pass it with the size, e.g. `format_hex_pretty(packet.data().data(), packet.data().size())`.

The helpers cut the link layer header out of `data()`: `l_field()`, `c_field()`, `manufacturer()` (three letters), `device_id()` (8 BCD digits), `meter_id()` (both together, e.g. `ABC12345678`), `version()`, `device_type()`, and `has_address()` for a packet that is too short for them.
The three that give text return a NUL terminated `std::array<char, N>`: `.data()` is the text for `%s`, and `std::string(packet.meter_id().data())` makes a `std::string` of it.

The name of the link mode is `LOG_STR_ARG(im891a_xl::packet_info_to_str(packet.packet_info))`.

The packets and the lines of a RAM image are logged at `VERBOSE`, as ESPHome logs received data.
`on_packet` and `on_image_line` may be given more than once.

## Bootloader and RAM images

The module is an STM32WLE5, and its ROM bootloader can load a program into the RAM and start it.
The flash is not touched, so IMST's firmware is back after any reset or power cycle.
This is how a Poseidon sniffer can run on the module, and it needs the module's read-out protection at level 0, which IMST's firmware leaves.

```yaml
im891a_xl:
  id: imst
  ram_images:
    - name: sniffer
      file: poseidon-sniffer-ram.bin   # relative to the configuration
      address: 0x20008000              # default
      verify: true                     # default, read it back before starting it
  on_image_line:
    - logger.log:
        format: "sniffer: %s"
        args: [line.c_str()]

button:
  - platform: template
    name: "Run the sniffer"
    on_press:
      - im891a_xl.load_image: sniffer
  - platform: template
    name: "Back to WM-Bus"
    on_press:
      - im891a_xl.reset: imst
```


| action | what it does |
|---|---|
| `im891a_xl.load_image: <name>` | Writes the image to the RAM, reads it back, and starts it. Restarts the module into the bootloader first if it is not there. |
| `im891a_xl.start_bootloader` | Only restarts the module into the bootloader. `via_hci: false` if it is there already, e.g. BOOT0 was held at its reset. `im891a_xl.start_bootloader: <id>` also works. |
| `im891a_xl.reset` | Resets the module, which starts its own firmware again, and the driver goes back to receiving. |

The module's firmware, and a running RAM image that implements it, restart into the bootloader on a request that is in IMST's example code but not in the specification: SAP 01, message F1.
So `load_image` also works while an image runs, to replace it, without a reset.
The driver checks that the image fits: it goes between `0x20008000` and `0x2000FFF0`, since the ROM bootloader uses the RAM below it and the module keeps a flag above it, and the reset vector in its second word has to point into the image.

### The UART changes format

The HCI protocol is not spoken while the bootloader or an image runs, and the UART is set up again at each step.
The baud rate stays what the `uart` has, the bootloader adapts to it.


| phase | format | protocol |
|---|---|---|
| the module's firmware | 8N1 | HCI in SLIP frames |
| the ROM bootloader | 8E1, even parity | the USART protocol of ST's AN3155 |
| a started image | 8N1, or `image_parity` | its own, line by line here |

The steps are:

1. The HCI request F1 goes out in 8N1, and the module answers and resets itself.
2. The UART is switched to 8E1, and after 100 ms the sync byte `0x7F` goes out until the bootloader answers, for up to 4 s.
3. Get and Get ID follow. The product ID has to be `0x0497`, the STM32WLE5, or the driver stops.
4. The image is written in chunks of 256 bytes, read back and compared, and started with Go at its address.
5. The UART is switched to `image_parity`, and what the image prints is forwarded line by line to `on_image_line`, and to the log at `VERBOSE`.
   Bytes that are not printable show as dots, so the lines are for text.

The module can also restart into its own firmware while an image runs, for example when the image's watchdog resets it.
The driver does not guess that from single bytes.
With `image_detect_restart` it feeds the output of the image through the SLIP decoder, and only a whole HCI frame with a valid CRC counts as the module's firmware: text cannot contain the frame delimiter, and random binary output passes a 16 bit CRC with a probability of about 1 in 65536 per candidate.
The driver then switches back to 8N1 and starts over with the module's firmware.
The firmware only says something unasked for after a restart if its startup indication is enabled in its system options, or once packet notifications are on, so a restart may also go unnoticed.
Then `im891a_xl.reset` is the way back, from an automation that knows the image should have printed something by now.
Set `image_detect_restart: false` for an image whose output you do not trust to stay clear of this.

If a step fails, the driver logs why, switches back to 8N1, and resets the module after 5 s, which needs the `reset_pin`.
Without it the module stays in the bootloader until it is power cycled.
The driver does nothing for receiving while all of this goes on.

## Wiring

The module is a 3.3 V part with a UART at 115200 baud, 8N1, and a reset pin that is active low.
The antenna and everything else is on the module.
The firmware can also drive LEDs and an RF switch from other pins, which this driver leaves off.

## Tests

The HCI codec is `hci.h`, and the bootloader client `bootloader.h`.
They are standard C++ only, so they build on the host, and constexpr, so the tests are `static_assert`s: a successful compilation means they all passed.
A check that fails inside a test function names itself in the compiler's error.
The names of the codes they use are tables in the component, and are not tested here.

```sh
cd tests/components/im891a_xl
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/im891a_xl -fsyntax-only hci_test.cpp
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/im891a_xl -fsyntax-only bootloader_test.cpp
```

`hci_test.cpp` checks the CRC and the SLIP framing against the frames of the specification, the encoding of the configuration, and the decoding of the answers.
The configuration test includes a literal `0xC0` in the data, which goes over the wire as `DB DC`.
It also checks the sizes: a frame fits a buffer of exactly its length and not one byte less, the longest message gets through and one byte more is dropped, a copied packet keeps its own bytes, and a firmware name that is too long is cut off.

The bootloader client does no I/O: it is given the bytes that arrive and the time, and says what to send.
`bootloader_test.cpp` runs it against a simulated ROM bootloader with its own RAM.
It covers the whole sequence for images of several sizes (with the padding of the last chunk), a bootloader that is late or never answers, a slow one whose reply to an extra sync byte arrives as a NACK, another product ID, read-out protection, memory that does not hold what was written, an address the bootloader refuses, a bootloader that stops answering in the middle of a write, one without the commands that are needed, a byte that is neither ACK nor NACK, and more to send than the buffer holds.
Only the simulation has been run against it: neither the bootloader client nor the component's state machine has been run against a module yet.

`test.esp32-idf.yaml` in the same directory builds the component with every option, both automations with and without `delay:`, and all actions.
It needs `poseidon-sniffer-ram.bin` next to it, a RAM image whose reset vector the configuration checks.

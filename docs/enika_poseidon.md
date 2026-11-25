# enika_poseidon

ESPHome external component that receives Enika Poseidon radio packets (868.3 MHz, 2-FSK, 38.4 kbit/s) through the stock `sx126x` component.
It was written for the SX1262 of the Arduino Nesso N1, and works with any `sx126x` radio.

## What it does

- Registers as a listener of the `sx126x` radio, and gets every packet whose CRC is good.
- Gives the packet to automations as the frame `<length> <data>`, the way the P8 TR USB dongle shows it.
- Decodes transmitter state messages (the key fob presses) into a `StateMessage` and gives it to `on_state_message`.
- Writes every packet to the log at `DEBUG`, with its RSSI and the one line summary: `-61 dBm 5F1B9A L..r. 13260`.
  At `VERBOSE` the frame with its length byte follows: `Frame: 0A 40 2C 5F 1B 9A 14 89 AB CD EF`.
  Set `logger:` `logs:` `enika_poseidon: VERBOSE` to see it.

Other packet types (device search, range scan, remote management,...) get a summary line, their bytes are in `frame`, and their fields are not decoded.

## Radio settings

The component refuses to build when these differ, since the packets would not arrive:


| `sx126x` option | value | note |
|---|---|---|
| `modulation` | `FSK` | |
| `bitrate` | `38400` | |
| `sync_value` | `[0xCE, 0x3C]` | the last preamble bit and the 15 bit sync word 9C78 |
| `payload_length` | `0` | variable length, a length byte follows the sync word |
| `crc_enable`, `crc_size` | `true`, `2` | |
| `crc_polynomial`, `crc_initial` | `0x8005`, `0xFFFF` | not inverted, `crc_inverted: false`. The CRC covers the length byte. |
| `whitening_enable` | `false` | |

These are not checked, and a weak transmitter may want other values:


| `sx126x` option | example | note |
|---|---|---|
| `frequency` | `868.3MHz` | |
| `deviation` | `20kHz` | the sniffer's guess for the CC1110 of the fob, which shows about 14.5 kHz in rtl_433. The datasheet treats it as a transmit parameter, so a receiver may ignore it. |
| `bandwidth` | `117_3kHz` | the sniffer tries 93_8kHz and 156_2kHz next to it for weak fobs |
| `preamble_size`, `preamble_detect` | `16`, `2` | bytes sent, and a 16 bit detector. Only the detector matters for receiving. |

## Configuration

```yaml
sx126x:
  id: radio
  # the pins, rf_switch and the TCXO of your board
  frequency: 868.3MHz
  modulation: FSK
  bitrate: 38400
  deviation: 20kHz
  bandwidth: 117_3kHz
  sync_value: [0xCE, 0x3C]
  payload_length: 0
  crc_enable: true
  crc_size: 2
  crc_polynomial: 0x8005
  crc_initial: 0xFFFF
  crc_inverted: false
  whitening_enable: false

enika_poseidon:
  id: poseidon_rx
  radio: radio
  on_packet:
    - logger.log:
        format: "%s %.0f dBm"
        args: [summary.c_str(), rssi]
  on_state_message:
    - if:
        condition:
          lambda: return message.id == 0x5F1B9A && message.left() && !message.repeat();
        then:
          - logger.log: "left button of the fob 0x5F1B9A"
```

Triggers, both optional, each one automation or a list of them:

- `on_packet`: `frame` (`<length> <data>`, without CRC), `rssi` (`float`, dBm) and `summary`.
  `frame` is a `std::span<const uint8_t>` and `summary` a `StringRef`, both only views into the receive buffer.
  Both have `size()`, `[]`, `begin()` and `end()`, `frame` has `data()` and `summary` has `c_str()`.
  An automation with an action that waits (`delay`, `wait_until`, `script.wait`,...) gets copies instead: `frame` is a `std::vector<uint8_t>` and `summary` a `std::string`.
  A lambda that only uses the members above works with both.
- `on_state_message`: `message` (an `enika_poseidon::StateMessage`) and `rssi`.
  The message holds copies of its bytes, so it may be used after a `delay`.

`poseidon_rx->get_packet_count()` is the number of packets received.

## StateMessage

Every bit of the frame is in some field.
`byte1`, `ctrl` and `flags` are the raw bytes, next to the fields cut out of them.


| field | meaning |
|---|---|
| `id` | transmitter ID, 24 bit, `0x5F1B9A` is shown as `5F1B9A` |
| `flags` | raw: `0x04` left button, `0x08` right, `0x10` repeat frame, `0x20` long press. Other bits (from 4-button transmitters?) are not supported yet. |
| `left()`, `right()`, `repeat()`, `long_press()` | the bits of `flags` |
| `counter`, `has_counter` | the press counter, unmasked. A press and its repeat frames have the same one. `0xFFFF` in an init frame. |
| `hash` | the 16 bit hash of the counter trailer |
| `cnt3` | byte 1 bits 6:4, the low 3 bits of the counter. In an init frame a separate init counter. |
| `msg_type` | byte 1 bits 3:0, always 0 so far |
| `init` | ctrl bit 7, a frame of the initialization message, which a double click sends |
| `trailer_format` | ctrl bits 5:4: 0 none, 1 one byte (`trailer_byte`), 2 counter and hash, 3 encrypted (the bytes are in `payload`) |
| `route()` | bytes that a repeater inserted after `ctrl`, up to 3 |
| `payload()` | bytes between the flags and the trailer |

`route()` and `payload()` return a `std::span<const uint8_t>`.
`trailer_format` compares with `enika_poseidon::TRAILER_NONE`, `TRAILER_ONE_BYTE`, `TRAILER_COUNTER_HASH` and `TRAILER_ENCRYPTED`, or with the numbers.

## Summary line

`<ID> <flags> <counter>` for a state message, fit for 27 columns of a 5x7 font.
The flags are a mask of 5 characters, a dot for each bit that is not set:


| character | bit of `flags` | meaning |
|---|---|---|
| `L` | `0x04` | left button |
| `R` | `0x08` | right button |
| `H` | `0x20` | long press (hold) |
| `r` | `0x10` | repeat frame, the first frame of a press has none |
| `?` | any other | a bit that is not known, `fXX` with the whole flags byte follows |

After the mask comes the counter, or `init` for an init frame, and `via` if a repeater is in the route.

Examples: `5F1B9A L.... 13260` (left button, first frame), `5F1B9A .RHr. 13261` (right button held, repeat frame), `5F1B9A LR... 13262` (both buttons), `5F1B9A L..r. init`.
Other packets: `search IDA-IDB`, `reply <ID>`, `range scan <n>`, `remote <type> req|rep <ID>`, and `?? type XX len N` for anything else.

## Tests

The tests are in `tests/components/enika_poseidon/`.

The decoder is `poseidon_packet.h`, standard C++ only, so it builds on the host, and constexpr, so the tests are `static_assert`s: a successful compilation means they all passed.

```sh
cd tests/components/enika_poseidon
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/enika_poseidon -fsyntax-only decode_test.cpp
```

`decode_dump.cpp` is a tool for the host: it reads one frame per line in hex from its input and prints every field the decoder finds.

```sh
g++ -std=c++20 -I../../../components/enika_poseidon decode_dump.cpp -o decode_dump
echo "0A 40 2C 5F 1B 9A 04 01 23 45 67" | ./decode_dump
```

`test.esp32-idf.yaml` builds every option and trigger for an ESP32-C6 with ESP-IDF.
It finds the component through a relative path: `esphome compile tests/components/enika_poseidon/test.esp32-idf.yaml`.

The driver allocates nothing on the heap for a packet: the frame, the decoded message and the summary are on the stack.
Only the radio's own `on_packet()` interface hands the data over in a `std::vector`, and an `on_packet` automation that waits gets its own copies.

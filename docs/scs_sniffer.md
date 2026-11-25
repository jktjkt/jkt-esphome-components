# scs_sniffer

ESPHome external component that decodes the BTicino (Legrand) SCS bus from a passive tap and gives every frame to an automation.
It sits on top of the stock `remote_receiver`, which timestamps the pulses with the ESP32's RMT peripheral.
It was written for the M5Stack Unit PoE-P4 (`U213`, ESP32-P4) with an RS-485-ISO unit (`U094`) turned into a tap, see the [scs-sniffer](https://github.com/jktjkt/scs-sniffer) project for the hardware modification.
It has only been built for the ESP32 and run on the ESP32-P4.

SCS is a 27 V DC bus that carries power and data on the same two wires.
Data is UART-like (9600 Bd, 8N1), but a `0` bit is only a short dip in the bus voltage, about 34 µs at the start of the 104 µs bit cell.
The tap turns each dip into an active low pulse of about 34 µs.
The component does not capture audio or video, only the data frames.
Even call metadata and door opening are personal data, mind that when you log or forward them.

## What it does

- Turns the pulse timings of each `remote_receiver` capture into bytes, and the bytes into `A8 ... A3` frames with a valid length and checksum.
  Frames are 7 or 11 bytes long depending on their first payload byte, and the payload can contain any value, `A3` included.
- Works out what a frame means, as far as it has been reverse-engineered: lighting and automation commands, state broadcasts, status requests, scenarios, door-entry rings and door lock release.
  Everything else, including all 11 byte frames, is `Kind::KIND_UNKNOWN`, and the raw bytes are still there.
- Gives each frame to `on_frame` and writes it to the log at `DEBUG`, e.g. `frame A8 91 03 60 08 FA A3: ring flat 03`.
- Writes a capture that did not decode cleanly to the log at `WARN`, with the raw capture, see below.

## Configuration

```yaml
remote_receiver:
  id: scs_rx
  pin:
    number: GPIO54
    inverted: true  # the tap's output is active low, the pulses must be marks
    mode:
      input: true
      pullup: true  # the on-board pull-up of the U094 is only 100 k
  filter: 2us  # the decoder rejects glitches shorter than 8 us itself
  idle: 2ms  # the longest gap inside a frame is about 0.8 ms
  rmt_symbols: 192  # no DMA, see below
  receive_symbols: 512  # the RX memory is refilled in ping-pong halves

scs_sniffer:
  id: scs
  receiver_id: scs_rx
  enable_pin: GPIO53  # Grove R↑T, held high: receiver on
  on_frame:
    - logger.log:
        format: "%s"
        args: [summary.c_str()]
    - if:
        condition:
          lambda: return message.kind == esphome::scs_sniffer::Kind::KIND_DOOR_LOCK;
        then:
          - logger.log:
              format: "door lock released by panel %02X"
              args: [message.src]
```


| option | |
|---|---|
| `receiver_id` | the `remote_receiver` that watches the tap, optional when there is only one |
| `enable_pin` | optional output pin, driven high at startup. On the U094 it is the Grove `R↑T` pin, which switches the receiver on. |
| `on_frame` | optional automations, see below |

The component refuses to build when the receiver cannot work:


| `remote_receiver` option | requirement |
|---|---|
| `pin` | `inverted: true` |
| `filter` | below 20 µs, the pulses are about 34 µs long |
| `idle` | at least 1 ms, a shorter one splits the frames |

The other options are not checked.
`use_dma` is left out of the example on purpose: on the ESP32-P4, ESP-IDF needs a cache-aligned buffer for it, which ESPHome does not allocate.
Without DMA, `rmt_symbols` is the size of the RX memory, and `receive_symbols` the buffer it is copied to.

## on_frame

Called for every valid frame.
Devices commonly send each frame up to three times, and every copy arrives here.
`A5` acknowledgements are not frames, they are only logged at `DEBUG`.


| variable | type | |
|---|---|---|
| `frame` | `esphome::scs_sniffer::Frame` | the whole frame, `A8` and `A3` included |
| `message` | `esphome::scs_sniffer::Message` | what the frame means, see below |
| `summary` | `StringRef`, or `std::string` after a delay (see below) | one line of text, e.g. `command 33 on (from 00)` |

`frame` has `begin()`, `end()` and `[]`, and its length in the member `frame.size`.
`summary` has `c_str()` and `size()`, and converts to `std::string`.
`frame` and `message` have a size fixed at compile time, and `summary` points to a buffer on the stack, so a frame allocates nothing on the heap.
That buffer only lives while the frame is handled.
So in an automation with a `delay`, `wait_until`, `script.wait` or another action that waits, `summary` is a `std::string` instead, a copy on the heap for every frame.
Put such actions into a separate automation, or copy what you need into a global first, to avoid that.

### Message


| field | |
|---|---|
| `kind` | an `esphome::scs_sniffer::Kind`, see the table |
| `dest` | the addressed device, for `KIND_COMMAND` and `KIND_STATUS_REQUEST`. The flat for `KIND_DOOR_RING`. |
| `src` | the sender, for `KIND_COMMAND`, `KIND_STATE`, `KIND_STATUS_REQUEST` and `KIND_SCENARIO`. The panel for `KIND_DOOR_LOCK`. |
| `value` | the action byte for `KIND_COMMAND` and `KIND_STATE`, the scenario number for `KIND_SCENARIO`, the last payload byte otherwise |


| `kind` | frame `A8 P0 P1 P2 P3 chk A3` | summary |
|---|---|---|
| `KIND_COMMAND` | `P0` dest, `P1` src, `P2` = `12`, `P3` action. A `P0` of `B1` addresses all devices. | `command 33 on (from 00)` |
| `KIND_STATE` | `P0` = `B8`, `P1` src, `P2` = `12`, `P3` action: a device broadcasting its new state | `state 33 off` |
| `KIND_STATUS_REQUEST` | `P0` dest, `P1` src, `P2` = `15` | `status request 33 (from 00)` |
| `KIND_SCENARIO` | `P0` src, `P2` = `14`, `P3` scenario number | `scenario 05 (from 21)` |
| `KIND_DOOR_RING` | `P0` = `91` (outdoor panel), `P1` flat, `P2` = `60` | `ring flat 03` |
| `KIND_DOOR_LOCK` | `P0` = `96`, `P1` = `80` + panel, `P2` = `6F`, `P3` = `A4` (open) or `A0` (release) | `door lock 20 open` |
| `KIND_UNKNOWN` | anything else | `unknown` |

The action byte of a command or a state is shown as `on`, `off`, `dim up`, `dim down`, `shutter up`, `shutter down`, `shutter stop`, `level 10%`...`level 100%` (low nibble `D`), `timed on XX`, `blink XX`, or `action XX` for any other value.
The mapping of the timed-on duration is not known.

There is no public specification, this follows what others reverse-engineered, see the credits in the [scs-sniffer](https://github.com/jktjkt/scs-sniffer) README.
For `KIND_UNKNOWN`, use the raw `frame`.

## Reading the diagnostic log

A capture that did not decode cleanly is logged like this:

```
[W][scs:096]: capture: 0 frames, 0 errors [], 1 stray bytes, 1 glitches, 6 durations
[W][scs:101]: bytes: FE
[W][scs:110]: raw[0]: 13 -95 8 -995 5 -2000
```

- `capture:` counts what the decoder made of one capture: complete frames, errors (by name), bytes outside a frame (`A5` acknowledgements excluded), and glitches (pulses shorter than 8 µs, ignored).
- `bytes:` every byte decoded from the capture, in order, whether or not it ended up in a frame.
  A byte whose dips were missed reads as `1` bits, so a lone start bit decodes as `FF`.
- `raw[n]:` the capture as `remote_receiver` delivers it, 24 values per line, `n` being the index of the first one.
  Each value is the duration of one level in µs, and the levels alternate: positive = mark = the bus dipped (the tap's output was low), negative = space = the bus was idle.
  These are durations, not timestamps, a pulse starts at the sum of all values before it.
  A capture starts with the first mark and ends with the idle that closed it, which is reported as the `idle:` setting (`-2000`).
  The resolution is 1 µs, and the RMT drops pulses shorter than `filter:` before they get here.

In the example, the start bit is a 13 µs pulse, the next pulse starts 108 µs (13 + 95) later, i.e., in bit 0, and the third starts 1111 µs in, after the stop bit.
It is only 5 µs long and counts as a glitch.
On a healthy tap, every pulse is about 30–34 µs long and starts close to a multiple of 104 µs (one bit at 9600 Bd) from the first.
The raw values can be pasted into a test and replayed with `Decoder::feed_marks_spaces()`.

The errors are `long low` (a pulse over 80 µs: a bus fault, wrong polarity), `bit phase` (an edge far from any bit boundary), `stop bit` (a pulse in the stop bit), `no end byte`, `checksum` and `truncated frame`.
After a bad frame, the decoder resumes at the next `A8` inside it.
The log tag is `scs`.

## Example device

The U213 has pre-rev-3 ESP32-P4 silicon, hence `engineering_sample`.
The tap's Grove `T↓R` pin goes to GPIO54 and `R↑T` to GPIO53.

```yaml
esphome:
  name: scs-tap

esp32:
  board: esp32-p4
  engineering_sample: true

ethernet:
  type: IP101
  mdc_pin: GPIO31
  mdio_pin: GPIO52
  clk:
    mode: CLK_EXT_IN
    pin: GPIO50
  phy_addr: 1
  power_pin: GPIO51

logger:

external_components:
  - source: github://jktjkt/jkt-esphome-components
    components: [scs_sniffer]

# remote_receiver and scs_sniffer as in the configuration above
```

## Tests

The decoder and the message layer are `scs_decoder.h` and `scs_protocol.h`, standard C++20 only, so they build on the host.
Their tests are `static_assert`s, so a successful compilation means they all passed:

```sh
cd tests/components/scs_sniffer
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/scs_sniffer -fsyntax-only decoder_test.cpp
g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror -I../../../components/scs_sniffer -fsyntax-only protocol_test.cpp
```

`tests/components/scs_sniffer/test.esp32-idf.yaml` builds the component with every option, for an ESP32-C6 with ESP-IDF:

```sh
cd tests/components/scs_sniffer
esphome compile test.esp32-idf.yaml
```

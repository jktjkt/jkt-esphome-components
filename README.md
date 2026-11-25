# ESPHome components

This repo focuses on WM-Bus energy monitoring, building automation, and M5Stack devices.

## Components

- AW32001 batery charger, for Arudino Nesso N1

- BQ27220 battery fuel gauge with all readable values, for Arduino Nesso N1

- `enika_poseidon`: Enika Poseidon 868 MHz receiver on top of the `sx126x` radio: packets, key fob state messages

- `im891a_xl`: IMST iM891A-XL WM-Bus radio module driver (UART, WM-Bus receive mode)

- `scs_sniffer`: BTicino SCS intercom/automation bus decoder for a passive tap, on top of `remote_receiver`: frames, commands, door rings

- `feature_gate`: turns groups of compiled-in components on or off per device from retained MQTT topics, cached in NVS

- `per_device_identity`: per-device MQTT identity for one firmware image: device id, broker address and client cert from NVS, enrolment over MQTT

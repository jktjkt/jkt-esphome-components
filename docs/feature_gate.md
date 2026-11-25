# feature_gate

ESPHome external component that turns groups of compiled-in components on or off per device, so that one firmware image can serve devices with different roles.
The choice comes from retained MQTT topics and is stored in ESPHome's preferences, which is NVS on an ESP32.
It needs the `mqtt` component.
Only ESP32 builds with ESP-IDF have been tried.
It was written for a fleet of M5Stack Unit PoE-P4 (`U213`, ESP32-P4), together with [per_device_identity](per_device_identity.md).

ESPHome has no way to tear a component down, and setting one up late breaks the setup order that buses and their devices rely on.
So a change is not a live toggle: the device stores the new choice and reboots.

## What it does

- Before any other component is set up (setup priority 1500, the same as the logger, above buses and `POWER`), loads the set of enabled features from the preferences.
  When nothing is stored, every feature uses its `default`.
  The set is stored under a key made from the names of all features, in their order.
  So when a firmware update adds, removes, renames or reorders features, the stored set is not used, and the device starts with the defaults until its retained config arrives.
- Parks the components of every disabled feature in `LOOP_DONE`.
  `App::setup()` then never calls their `setup()`, and the main loop never calls their `loop()`.
  They are not marked failed, so there is no error status.
- Once MQTT is connected, subscribes to `<topic_prefix>/<topic>/+`, with QoS 1, and takes the feature from the last level of the topic.
  A message for a name that is no feature is logged and ignored.
  The topic prefix is read late, because `per_device_identity` can change what the client uses.
- Publishes the set that is running in this boot, retained, to `<topic_prefix>/features`, e.g. `{"dlms":1,"ups":0,"wmbus":0}`.
- When a received value differs from what is running, waits for `reboot_delay`, stores all features and reboots once.
  If storing fails, it logs an error and does not reboot, since the device would come back with the old set.
  The retained messages of all features arrive back to back after connecting, so the delay makes them one reboot.
  A value that reverts the difference within the delay cancels the reboot.

A new device boots with the defaults, gets its retained config and reboots once.
A device without a network keeps its last choice.

## Configuration

```yaml
mqtt:
  id: mqtt_client
  # ...

feature_gate:
  mqtt_id: mqtt_client
  features:
    - name: dlms
      components: [grove_uart_dlms, dlms_hub]
    - name: ups
      default: true
      components: [grove_uart_ups, ups_modbus, ups_ctrl, ups_battery]
```


| option | |
|---|---|
| `mqtt_id` | the `mqtt` component, found automatically when there is only one |
| `topic` | optional, default `config`. The middle part of `<topic_prefix>/<topic>/<feature>`, up to 64 characters of `[a-z0-9_]`. |
| `reboot_delay` | optional, default `3s`. How long a pending change must stay before the device stores it and reboots. |
| `features` | required, the list below, up to 32 |


| feature option | |
|---|---|
| `name` | required, unique, 1 to 15 characters of `[a-z0-9_]`. It is the last part of the topic. |
| `default` | optional, default `false`. Used while nothing is stored, and when the retained message is cleared. |
| `components` | required, ids of the components of this feature |

## Choosing a feature

```sh
mosquitto_pub ... -r -t u213-a1b2c3/config/dlms -m 1
mosquitto_pub ... -r -t u213-a1b2c3/config/ups  -m 0
```


| payload | |
|---|---|
| `1`, `true`, `on` | enable |
| `0`, `false`, `off` | disable |
| empty | back to the feature's `default`, which is what clearing the retained message sends |

Anything else is ignored with a warning.
The component only reads these topics, so make sure that the broker lets only an administrator write them.

## Rules for the YAML

- List every `Component` of a feature: buses, hubs and platform components that are `Component`s, e.g. a `ModbusSensor`.
  Anything that calls into a gated component must be gated too, since gated components are never set up.
- A component whose setup priority is above 1500 has already been set up when the gate runs.
  The gate logs an error and leaves it running.
- Features may share pins if you set `allow_other_uses: true` on them.
  That only silences ESPHome's compile-time check, so never enable two features that use the same pins.
- ESPHome still calls `dump_config()` of gated components, so their config appears in the log even though they do not run.

## For lambdas

`id(gate).is_enabled("dlms")` tells whether the feature runs in this boot.
It returns `false` for an unknown name.

## Memory

The gate allocates nothing on the heap after `setup()`, except that storing a change and rebooting goes through the preferences, which may allocate.
The features and their components are static tables from the code generator, and the state of the features is a bit mask, so there can be up to 32 features.
The topics and the JSON are built in buffers of a fixed size.
The subscription keeps no topic string: the MQTT client builds the topic when it needs it.

## Tests

`tests/components/feature_gate/test.esp32-idf.yaml` is a build test with every option.
It gates two UART buses and an `interval`, and uses `is_enabled()` in a lambda.
Build it from that directory:

```sh
esphome compile test.esp32-idf.yaml
```

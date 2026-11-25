# per_device_identity

ESPHome external component that gives every device running the same firmware image its own MQTT identity.
The device id comes from the MAC address, the broker address can be per device, and the client certificate lives in a dedicated NVS partition.
A device without a certificate generates its own key and enrols over MQTT.
It needs an ESP32 with ESP-IDF and the `mqtt` component, and mbedTLS with CSR writing.
It needs ESP-IDF 6 or newer (mbedTLS 4).
ESP-IDF leaves the CSR writer out by default, so the component turns on `CONFIG_MBEDTLS_X509_CREATE_C` and `CONFIG_MBEDTLS_X509_CSR_WRITE_C`.
It was written for a fleet of M5Stack Unit PoE-P4 (`U213`, ESP32-P4), together with [feature_gate](feature_gate.md).

## What it does

- Uses the ESPHome device name, which with `name_add_mac_suffix` is `<name>-<last 3 MAC bytes>`, as the device id.
  The id is the MQTT client id, and ESPHome already uses it as the topic prefix.
  The certificate CN is expected to be the same.
- With `broker_domain`, connects to `<id>.<broker_domain>`, e.g. `u213-a1b2c3.mqtt.example.org`, instead of the `mqtt:` broker.
  ESPHome gives the name, not the resolved address, to esp-tls, so the broker certificate needs a matching SAN such as `*.mqtt.example.org`.
- Reads `cert` and `key` (PEM) from the NVS partition `partition`, namespace `identity`, and hands them to the MQTT client.
  Both present: the device is enrolled and the component does nothing else.
- Without them, the `client_certificate` of the `mqtt:` block stays in place as a bootstrap identity, and the device enrols:
  - It generates a P-256 key and keeps it in NVS as `pendkey`, so that reboots keep asking for the same key.
  - It connects to the broker on `enrol_port` as client `enrol-<id>`.
    That client id differs on purpose: an enrolled device's client id is forced to its certificate CN, and the same id would kick it off.
  - It subscribes to `<id>/enrol/cert` and publishes a CSR with `CN=<id>` to `<id>/enrol/csr`, repeating it every `csr_interval`.
    The first CSR goes out 2 to 4 s after the connection is up, so that the subscription is in place before the answer comes.
  - A certificate that arrives on `<id>/enrol/cert` and matches the key is stored with the key as `cert` and `key`, `pendkey` is erased, and the device reboots.
    ESPHome hands the TLS credentials to the MQTT backend only on the first connect, which is why it reboots.
  - A certificate that does not match the key is ignored.

No private key ever travels over MQTT, since everybody holding the bootstrap key could listen.
Something on the broker side must sign the CSRs and decide who may enrol, this repo has none.
The NVS partition can instead be filled on a host and flashed next to the image.

The partition is opened with no auto-erase, so a damaged one fails the component instead of destroying an enrolled key.
For the same reason, a `cert` or `key` that is there but cannot be read fails the component instead of starting a new enrolment.

## Configuration

```yaml
esphome:
  name: u213
  name_add_mac_suffix: true

esp32:
  partitions:
    - name: identity
      type: data
      subtype: nvs
      size: 0x6000

mqtt:
  id: mqtt_client
  broker: mqtt.example.org  # only a placeholder with broker_domain
  port: 8883
  certificate_authority: !secret mqtt_ca
  client_certificate: !secret mqtt_bootstrap_cert
  client_certificate_key: !secret mqtt_bootstrap_key
  discovery: false

per_device_identity:
  mqtt_id: mqtt_client
  broker_domain: mqtt.example.org
```


| option | |
|---|---|
| `mqtt_id` | the `mqtt` component, found automatically when there is only one |
| `partition` | optional, default `identity`, at most 15 characters. The NVS partition with the certificate, which has to be in the partition table. |
| `csr_interval` | optional, default `60s`. How often an unenrolled device asks for a certificate. The component checks every 2 s, so the interval is rounded up to a multiple of 2 s. |
| `enrol_port` | optional, default `8884`. The broker port for bootstrap connections. |
| `broker_domain` | optional. Connect to `<id>.<broker_domain>`, with the full device id. |

The component refuses to build unless:

- `esphome:` has `name_add_mac_suffix: true`, since the device id is built from it.
- `mqtt:` has no `topic_prefix` other than the device name, since a broker ACL that pins a device to `<cert CN>/#` needs the topics to live there.

## NVS layout

Partition `partition`, namespace `identity`:


| key | |
|---|---|
| `cert` | PEM client certificate, used when `key` is present too |
| `key` | PEM private key |
| `pendkey` | PEM key waiting for a certificate, only in bootstrap mode |

## Things to know

- Treat the bootstrap certificate as public, it is the same in every image.
  It should be allowed to do nothing but enrol.
- The 3 bytes of the MAC are unique within one Espressif MAC block, not across blocks.
  Two devices with the same suffix would share an id.
- A leaked key is a matter for revocation on the broker.
  An open connection survives a CRL update on some brokers, so restart them.
- The component runs at the default setup priority (`DATA`), before `mqtt` has initialised its backend.
- The MQTT backend keeps only pointers to the certificate and key, so the component keeps them in members that are never reassigned.

## Memory

What goes to the heap is what has a size that is only known at run time and lives as long as the firmware: the certificate and key read from NVS, and in bootstrap mode the pending key and the CSR.
Each is allocated once, at setup.
The MQTT client keeps its own copies of the client id and the broker address.
The topics and names are built in stack buffers when they are needed, and the subscription keeps no topic string.
After setup, only a received certificate is parsed on the heap, by mbedTLS.

## Tests

The build test is `tests/components/per_device_identity/test.esp32-idf.yaml`, with every option.
It pulls the component from this repo with `external_components`:

```sh
cd tests/components/per_device_identity
esphome compile test.esp32-idf.yaml
```

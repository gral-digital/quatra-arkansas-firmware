# Quatra CM5 ⇄ webapp bridge

A small Python daemon that runs on the Raspberry CM5 and shuttles
JSON messages between the ESP32-S3 (over UART) and the webapp
(over a pluggable transport — **HTTPS**, **MQTT**, or **stdout**
for bench bring-up).

This is the companion to the
[Quatra Arkansas firmware library](../components/quatra/README.md):
the firmware emits/consumes the protocol defined in PRD §8 on its
UART; this bridge carries those JSON frames to and from your
backend without modifying them.

```
┌────────────┐  RS485/LoRa     ┌────────────────┐    UART     ┌─────────────┐  HTTPS/MQTT   ┌────────────┐
│  Sensors   │───────────────▶│   ESP32-S3     │───────────▶│  CM5 bridge │──────────────▶│  Webapp    │
│  & relays  │◀───────────────│  (firmware)    │◀───────────│   (this)    │◀──────────────│  backend   │
└────────────┘                 └────────────────┘  JSON line  └─────────────┘   JSON line   └────────────┘
```

---

## Why a bridge

PRD v1.1 amendments §2 puts WiFi and the cloud transport on the
CM5, not on the ESP32. The ESP32 only needs to push/pull JSON over
UART — a self-contained, testable surface. The CM5 then handles:

* Internet connectivity (your existing networking on Raspberry Pi OS)
* TLS, retries, auth tokens, broker credentials
* Whatever your webapp expects on the wire (REST / MQTT / WS)

If your webapp later changes from REST to MQTT, only this bridge
changes; the firmware is untouched.

---

## What it does (precisely)

* Opens `/dev/ttyAMA0` (or any port you configure), reads lines
  terminated by `\n`, validates that each line is well-formed JSON,
  and forwards it to the configured transport.
* Receives JSON messages from the transport, validates them, and
  writes them back to the UART with a trailing `\n`.
* Reconnects to the serial port automatically if it disappears (USB
  unplug, ESP32 reset, kernel module reload).
* Bounded outbound queue (256 by default): when the cloud is down,
  the newest messages win — the oldest are dropped and counted.
* Emits a one-line stats log every 60 seconds (`uart_in`, `uart_out`,
  `pub_ok`, `pub_fail`, `q_drops`, `invalid_json_up/down`).

### What it does NOT do

* It does not interpret the JSON. Schemas are owned by the firmware
  (PRD §8) and by your webapp. The bridge only validates that each
  line is parseable JSON.
* It does not buffer to disk — only in RAM. If you need durability,
  use the MQTT transport pointing at a broker with a persisted queue
  (e.g. EMQX, Mosquitto with `persistence true`).
* It does not provision WiFi. Your CM5 image must already be online.

---

## Install on the CM5 (production)

```bash
# On the CM5, as root, after cloning the firmware repo:
cd quatra-arkansas-firmware/cm5-bridge
sudo ./systemd/install.sh

# Edit the freshly-installed config (transport, device_id, creds):
sudo nano /etc/quatra/bridge.toml

# Enable and start:
sudo systemctl enable --now quatra-cm5-bridge.service
journalctl -u quatra-cm5-bridge -f
```

The installer creates a `quatra` system user (member of `dialout`),
sets up a venv under `/opt/quatra-cm5-bridge/.venv`, installs the
dependencies, drops the systemd unit, and seeds `/etc/quatra/bridge.toml`
from `config.example.toml` if no config exists.

Re-running `install.sh` is safe: it upgrades the venv in place and
leaves your existing config untouched.

---

## Run locally for bench testing (development)

```bash
cd cm5-bridge
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt

# Bench mode — print everything the ESP32 says, no cloud needed:
cp config.example.toml bridge.toml      # default transport is "stdout"
.venv/bin/python -m quatra_cm5_bridge --config bridge.toml
```

To test without a real ESP32 attached, point the bridge at a virtual
serial port. macOS:

```bash
# Terminal 1: create a virtual pair
socat -d -d PTY,raw,echo=0 PTY,raw,echo=0
# Note the two /dev/ttys00X paths it prints. Use one as bridge port,
# write JSON to the other:
echo '{"ts":1,"sensors":[10,20,30]}' > /dev/ttys003
```

---

## Configuration

All settings live in a single TOML file (see `config.example.toml`
for the full schema). Quick reference:

| Section   | Key                  | Default            | Notes                                                                    |
| --------- | -------------------- | ------------------ | ------------------------------------------------------------------------ |
| `[bridge]`| `device_id`          | _required_         | Used in MQTT topics, `X-Device-Id` HTTP header, and logs                |
|           | `transport`          | `"stdout"`         | One of `http`, `mqtt`, `stdout`                                          |
|           | `log_level`          | `"INFO"`           | Standard Python levels                                                   |
| `[uart]`  | `port`               | `/dev/ttyAMA0`     | Use `/dev/ttyUSB0` for USB-UART adapters                                 |
|           | `baudrate`           | `115200`           | Must match the firmware                                                  |
|           | `bytesize` / `parity` / `stopbits` | `8 / N / 1` | Match the firmware                                                       |
|           | `read_timeout_s`     | `1.0`              | Per-read serial timeout                                                  |
|           | `reconnect_delay_s`  | `2.0`              | Backoff between reconnect attempts                                       |
| `[http]`  | `base_url`           | _required for http_| Without trailing slash                                                   |
|           | `publish_path`       | `/messages`        | POSTed JSON body, one message per request                                |
|           | `poll_path`          | `/commands`        | Periodically GET'd                                                       |
|           | `poll_interval_s`    | `2.0`              | Lower it if your webapp does not support long-poll                       |
|           | `request_timeout_s`  | `10.0`             | Per-request                                                              |
|           | `auth_token`         | _optional_         | Sent as `Authorization: Bearer …`                                        |
|           | `verify_tls`         | `true`             | Set false for self-signed test certificates                              |
| `[mqtt]`  | `host` / `port`      | `localhost / 1883` |                                                                          |
|           | `use_tls`            | `false`            | Set true for mTLS / standard TLS broker (default port becomes `8883`)    |
|           | `username` / `password` | _optional_      |                                                                          |
|           | `client_id`          | `quatra-cm5-bridge`|                                                                          |
|           | `publish_topic`      | `quatra/device/{device_id}/up`  | `{device_id}` is substituted                              |
|           | `subscribe_topic`    | `quatra/device/{device_id}/cmd` | `{device_id}` is substituted                              |
|           | `qos`                | `1`                | 0, 1, or 2                                                               |
|           | `keepalive_s`        | `60`               |                                                                          |

Env-var fallback (used when no `--config` is passed):
`QUATRA_DEVICE_ID`, `QUATRA_TRANSPORT`, `QUATRA_UART_PORT`,
`QUATRA_UART_BAUD`, `QUATRA_HTTP_BASE_URL`, `QUATRA_HTTP_TOKEN`,
`QUATRA_MQTT_HOST`, `QUATRA_MQTT_PORT`, `QUATRA_MQTT_USER`,
`QUATRA_MQTT_PASS`, `QUATRA_LOG_LEVEL`.

---

## HTTP transport contract

Your webapp backend needs to expose two endpoints (paths are
configurable):

### `POST {base_url}{publish_path}`

* **Request body**: one JSON object per request — exactly what the
  firmware emitted, byte-for-byte. See PRD §8 / firmware
  [protocol catalog](../components/quatra/README.md#json-protocol)
  for the 5 outbound message types.
* **Headers**: `Content-Type: application/json`, `X-Device-Id: <device_id>`,
  and (if configured) `Authorization: Bearer <token>`.
* **Expected response**: any 2xx — body is ignored.
* On non-2xx or network failure, the message is counted as failed
  (`pub_fail` in stats) and dropped. We do NOT retry: if you need
  retries, return 2xx after enqueueing on the backend side.

### `GET {base_url}{poll_path}?device_id=<device_id>`

* Called every `poll_interval_s`.
* **Expected response**:
  * `204 No Content` (or empty body) when no commands queued, OR
  * a JSON array of command objects, OR
  * a single JSON object (treated as one-element array), OR
  * NDJSON (one JSON object per line).
* See PRD §8 / firmware
  [protocol catalog](../components/quatra/README.md#json-protocol)
  for the 6 inbound command shapes.
* On non-2xx: logged and skipped. The bridge keeps polling.

---

## MQTT transport contract

* **Uplink topic**: `publish_topic` with `{device_id}` substituted.
  One JSON object per MQTT message, payload is the raw JSON string.
* **Downlink topic**: `subscribe_topic` with `{device_id}` substituted.
  Bridge subscribes at the configured QoS. Each message payload must
  be one JSON object — it is delivered as-is to the UART.
* TLS: enable `use_tls = true` and use a standard CA bundle (system
  CA store is used). For mTLS, edit `transports/mqtt.py` to call
  `tls_set(certfile=…, keyfile=…)`.

---

## Extending — add a new transport

Drop a new module under `quatra_cm5_bridge/transports/`, e.g.
`websocket.py`, implementing the `Transport` interface:

```python
from .base import Transport, CommandHandler

class WebsocketTransport(Transport):
    def __init__(self, cfg, device_id): ...
    def start(self, on_command: CommandHandler) -> None: ...
    def stop(self, timeout_s: float = 5.0) -> None: ...
    def publish(self, json_line: str) -> bool: ...
```

Register it in `bridge.build_transport()` and add a config section
in `config.py`. Add a fake-backed unit test in `tests/` mirroring
`test_stdout_transport.py`.

---

## Tests

```bash
cd cm5-bridge
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/pytest -v
```

22 tests covering: config parsing, UART framing (single, multi,
split, CRLF, oversize, reconnect, callback-exception), bridge
end-to-end (uplink/downlink happy paths, invalid JSON dropped in
both directions, publish failure counted, queue overflow drops
oldest), stdout transport.

The suite uses an in-memory `FakeSerial` and `FakeTransport`, so it
runs on macOS / Linux / CI without any hardware, broker, or web
service.

---

## Troubleshooting

| Symptom                                | Likely cause                                                              | Fix                                                                                  |
| -------------------------------------- | ------------------------------------------------------------------------- | ------------------------------------------------------------------------------------ |
| `uart open failed … Permission denied` | User is not in the `dialout` group                                        | `sudo usermod -aG dialout quatra` then re-login                                      |
| `uart open failed … No such device`    | Wrong port or UART not enabled in `/boot/firmware/config.txt`             | `dtoverlay=disable-bt` + `enable_uart=1` on Pi OS, then reboot                       |
| `invalid_json_up` keeps growing        | ESP32 is logging plain text on the UART instead of JSON                   | Move firmware logs to USB CDC or to UART1; keep UART0 JSON-only                      |
| `pub_fail` is high on http transport   | Backend returns 4xx/5xx, or TLS handshake failing                         | Inspect log line for status code; try `verify_tls = false` only to isolate           |
| MQTT connects then drops               | `keepalive_s` too high for broker / NAT idle timeout                      | Lower to 30                                                                          |
| Bridge keeps restarting                | Unhandled exception at startup (config error usually)                     | `journalctl -u quatra-cm5-bridge -e` for the traceback                               |

---

## License

Same license as the parent firmware library (proprietary, Gral Digital).

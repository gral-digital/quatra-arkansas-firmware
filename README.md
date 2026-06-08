# Quatra Arkansas — Firmware library + CM5 bridge

Two deliverables for the Quatra Arkansas smart-irrigation controller:

1. **`components/quatra/`** — ESP-IDF component: pure C99 implementation of
   the irrigation algorithm and the webapp JSON protocol. Hardware-agnostic;
   the integrator owns sensor acquisition (LoRaWAN soil data, RS-485 weather
   station, RS-485 relay board), NVS persistence and FreeRTOS task wiring.
2. **`cm5-bridge/`** — Python daemon that runs on the Raspberry CM5 and
   shuttles the firmware's JSON messages between the ESP32-S3 UART and the
   webapp (HTTPS, MQTT, or stdout — pluggable). Ships with a systemd unit
   and 22 unit tests.

## Documentation — start here

- **[ESP-IDF integration guide](components/quatra/README.md)** — full
  reference for the firmware library: step-by-step integration,
  function-by-function API reference, JSON protocol catalog with payload
  schemas for every message, default configuration, integration
  responsibilities, known limitations.
- **[Reference firmware example](examples/main.c)** — complete commented
  integration skeleton (irrigation tick, daily ET0 task, UART RX
  dispatcher). Copy into your project's `main/` and fill in the hardware
  stubs marked `TODO`.
- **[CM5 bridge guide](cm5-bridge/README.md)** — install on the CM5, pick
  a transport, run as a systemd service. Covers HTTP / MQTT / stdout
  configuration, extension API, troubleshooting.

## Repository layout

```
.
├── components/
│   └── quatra/                ESP-IDF component (drop into your IDF project)
│       ├── CMakeLists.txt
│       ├── idf_component.yml
│       ├── README.md          Integration guide (start here)
│       ├── include/           Public headers (5 files)
│       └── src/               Implementations (3 files)
├── examples/
│   └── main.c                 Reference firmware integration for ESP-IDF
├── cm5-bridge/                Raspberry CM5 ⇄ webapp bridge (Python)
│   ├── README.md              Install / config / extension guide
│   ├── quatra_cm5_bridge/     Package (config, uart_link, bridge, transports)
│   ├── systemd/               Unit file + install.sh
│   ├── tests/                 22 pytest unit tests (mock serial + transport)
│   ├── requirements.txt
│   └── config.example.toml
└── tests/                     Firmware host-side unit tests (no ESP-IDF needed)
    ├── Makefile
    ├── test_et0.c             FAO-56 ET0 (incl. Bangkok reference case)
    ├── test_zone.c            State machine and stop conditions
    ├── test_uart.c            JSON formatters and parser
    └── vendor/                Vendored cJSON v1.7.18 (MIT)
```

## Build status

| Module           | Status                              | Notes                       |
|------------------|-------------------------------------|-----------------------------|
| `quatra_config`  | done                                | constants only              |
| `quatra_types`   | done                                | no FreeRTOS / driver types  |
| `quatra_et0`     | done — full FAO-56, 13 steps        | pure math, host-tested      |
| `quatra_zone`    | done — FSM + control loop           | pure function, host-tested  |
| `quatra_uart`    | done — 11 JSON message types        | format + parse, host-tested |

Host tests: **35 suites, ~120 assertions, 0 failures**, built with
`-Wall -Wextra` under `gcc/clang`.

## Quick start — on ESP-IDF

1. Drop `components/quatra/` under your IDF project's `components/`
   directory.
2. Add `quatra` to your component's `REQUIRES` in `main/CMakeLists.txt`.
3. Copy [`examples/main.c`](examples/main.c) into your `main/` directory
   and fill in the eleven `TODO` hardware stubs at the top of the file.
4. Build with `idf.py build` for any ESP32 / S2 / S3 / C3 / C6 target.

For a full walkthrough see the
**[Step-by-step integration](components/quatra/README.md#step-by-step-integration)**
section of the integration guide.

## Quick start — CM5 bridge

```sh
# On the CM5, as root:
cd cm5-bridge
sudo ./systemd/install.sh
sudo nano /etc/quatra/bridge.toml    # pick transport + credentials
sudo systemctl enable --now quatra-cm5-bridge.service
journalctl -u quatra-cm5-bridge -f
```

For bench testing without a CM5, run locally with the `stdout` transport
to see live messages from the ESP32 — see
[`cm5-bridge/README.md`](cm5-bridge/README.md#run-locally-for-bench-testing-development).

## Quick start — firmware host tests (Mac / Linux)

```sh
cd tests
make test
```

Builds and runs every firmware unit test. cJSON is vendored, so no
system-wide library install is required.

## Quick start — CM5 bridge tests (Mac / Linux)

```sh
cd cm5-bridge
python3 -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/pytest -v
```

22 tests covering UART framing, bridge orchestration and config loading,
using in-memory fakes (no serial port, broker, or web service required).

## License

Source code under `components/quatra/`, `cm5-bridge/` and `tests/`
(excluding `tests/vendor/`) is proprietary to the project owner. The
vendored cJSON library carries its own MIT license — see
`tests/vendor/cJSON.LICENSE`.

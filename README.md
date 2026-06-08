# Quatra Arkansas — Firmware library

ESP-IDF component implementing the **irrigation algorithm** and the **webapp
JSON protocol** for the Quatra Arkansas smart-irrigation controller.

Hardware-agnostic by design: the integrator owns all hardware acquisition
(LoRaWAN soil data, RS-485 weather station, RS-485 relay board), WiFi
connectivity, NVS persistence and FreeRTOS task wiring. This library
provides pure C99 functions that map sensor inputs to irrigation decisions
and JSON payloads.

## Documentation — start here

- **[Integration guide](components/quatra/README.md)** — full reference for
  integrators: step-by-step integration, function-by-function API reference
  (parameters / return / notes), JSON protocol catalog with payload schemas
  for every message, default configuration, integration responsibilities,
  known limitations.
- **[Reference example](examples/main.c)** — a complete, commented
  integration skeleton (irrigation tick, daily ET0 task, UART RX
  dispatcher). Copy into your project's `main/` directory and fill in the
  hardware stubs marked `TODO`.

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
│   └── main.c                 Reference integration for ESP-IDF
└── tests/                     Host-side unit tests (no ESP-IDF needed)
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

## Quick start — on host (Mac / Linux)

```sh
cd tests
make test
```

Builds and runs every unit test. cJSON is vendored, so no system-wide
library install is required.

## License

Source code under `components/quatra/` and `tests/` (excluding `tests/vendor/`)
is proprietary to the project owner. The vendored cJSON library carries its
own MIT license — see `tests/vendor/cJSON.LICENSE`.

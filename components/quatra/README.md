# Quatra Arkansas — firmware library (v1.1)

ESP-IDF component that implements two things, and only two things, per the
PRD v1.1 amendments:

1. The **irrigation algorithm** (FAO-56 ET0 + per-zone state machine), as a
   set of pure C functions.
2. The **webapp JSON protocol** (de)serialization for the ESP32 ↔ CM5 link,
   as a set of pure string formatters and a JSON parser.

No GPIO, no UART driver, no NVS, no WiFi, no FreeRTOS task wiring. The
integrator owns hardware and scheduling and calls Quatra's functions from
its own main application.

## Module map

| Header             | Responsibility                                          | PRD ref     |
|--------------------|---------------------------------------------------------|-------------|
| `quatra_config.h`  | Algorithm constants and defaults                        | §12         |
| `quatra_types.h`   | Shared structs and enums (no FreeRTOS / driver handles) | §6.2 (subset) |
| `quatra_et0.h`     | FAO-56 Penman-Monteith ET0 (pure math)                  | §5.2        |
| `quatra_zone.h`    | Per-zone state machine (`zone_result_t` returned)       | §4.2, §5.5  |
| `quatra_uart.h`    | JSON formatters + parser for the webapp link            | §8          |

## Integration

```c
#include "quatra_et0.h"
#include "quatra_zone.h"
#include "quatra_uart.h"

void irrigation_tick(void)
{
    /* Inputs come from your code (LoRaWAN, RS-485, RTC, NVS, …). */
    float psi[3]     = read_watermark_cb(zone);
    float weights[3] = load_weights(zone);

    quatra_zone_result_t r = quatra_zone_update(
        zone, psi, weights, mad_cb, etacc_mm,
        flow_m3h, area_m2, elapsed_s, mode);

    if (r.valve_open) set_relay(zone, true);
    else              set_relay(zone, false);

    save_etacc_to_nvs(zone, r.ETacc_mm);

    char buf[1024];
    int n = quatra_uart_format_valve_state(buf, sizeof(buf), epoch_now,
                                           zone, r.valve_open, r.reason,
                                           r.D_applied_mm, r.ETacc_mm);
    if (n > 0) uart_write_bytes(MY_UART_PORT, buf, n);
}
```

## Configuration

All defaults live in `include/quatra_config.h`. The only one likely to need
local tuning is:

- `QUATRA_PARALLEL_ZONES` — max simultaneous irrigations (default 1).

Per-zone agronomic parameters (Kc, MAD, area, flow, weights) and the global
latitude / anemometer height are passed in as **function arguments**, so they
can be reconfigured at runtime by the integrator without rebuilding the
firmware.

## Host-side unit tests

```sh
cd tests
make test
```

The Quatra library is pure C99 with no hardware dependencies, so the test
suite compiles and runs on macOS and Linux. cJSON is vendored under
`tests/vendor/` — no system-wide library install is required.

Coverage today: 32 tests, 110+ assertions, 0 failures.

## Notes on the v1.1 scope reduction

Earlier drafts of this component included modules for soil acquisition,
weather acquisition, valve GPIO control, NVS persistence, WiFi provisioning
and a FreeRTOS task wiring layer. Those modules have been removed per PRD
v1.1 amendments §2 — the integrator owns them in their main application.
The hardware data path is:

```
Watermark sensors → 200SS-VA3/SDI adapter → Dragino LoRaWAN node →
    LoRaWAN gateway → (network) → CM5 / webapp
Weather station   → RS-485 Modbus → RS-485-UART adapter → ESP32-S3 UART
Relay board       → RS-485 Modbus → RS-485-UART adapter → ESP32-S3 UART
ESP32-S3          ↔ UART ↔ Raspberry CM5 ↔ internet ↔ WebApp
```

All readings reach this library as plain `float` values; all outputs leave
it as plain return values and JSON strings.

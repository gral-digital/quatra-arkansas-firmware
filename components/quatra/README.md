# Quatra Arkansas — firmware library (v1.1)

ESP-IDF component that implements two things, and only two things:

1. The **irrigation algorithm** (FAO-56 ET0 + per-zone state machine), as a
   set of pure C functions.
2. The **webapp JSON protocol** (de)serialization for the ESP32 ↔ CM5 link,
   as a set of pure string formatters and a JSON parser.

No GPIO, no UART driver, no NVS, no WiFi, no FreeRTOS task wiring. The
integrator owns hardware and scheduling and calls Quatra's functions from
its own main application.

## Table of contents

1. [Quick start](#quick-start)
2. [Module map](#module-map)
3. [API reference](#api-reference)
4. [JSON protocol catalog](#json-protocol-catalog)
5. [Default configuration](#default-configuration)
6. [Integration responsibilities](#integration-responsibilities)
7. [Known limitations](#known-limitations)
8. [Host-side unit tests](#host-side-unit-tests)
9. [Hardware data path](#hardware-data-path)
10. [Versioning and license](#versioning-and-license)

---

## Quick start

Drop `components/quatra/` under your IDF project's `components/` directory,
add `quatra` to your component's `REQUIRES`, and call the algorithm
functions from your own task. Minimal per-zone tick:

```c
#include "quatra_et0.h"
#include "quatra_zone.h"
#include "quatra_uart.h"

void irrigation_tick(uint8_t zone)
{
    /* Inputs come from your code (LoRaWAN, RS-485, RTC, NVS, …). */
    float psi[3]     = read_watermark_cb(zone);
    float weights[3] = load_weights(zone);

    quatra_zone_result_t r = quatra_zone_update(
        zone, psi, weights, mad_cb, etacc_mm,
        flow_m3h, area_m2, elapsed_s, mode);

    set_relay(zone, r.valve_open);
    save_etacc_to_nvs(zone, r.ETacc_mm);

    char buf[1024];
    int n = quatra_uart_format_valve_state(
        buf, sizeof(buf), epoch_now,
        zone, r.valve_open, r.reason,
        r.D_applied_mm, r.ETacc_mm);
    if (n > 0) uart_write_bytes(MY_UART_PORT, buf, n);
}
```

The functions are pure: no globals, no statics, no allocation after init.
You can call them from multiple tasks in parallel without locks.

---

## Module map

| Header             | Responsibility                                          |
|--------------------|---------------------------------------------------------|
| `quatra_config.h`  | Algorithm constants and tunable defaults                |
| `quatra_types.h`   | Shared structs and enums (no FreeRTOS / driver handles) |
| `quatra_et0.h`     | FAO-56 Penman-Monteith ET0 (pure math)                  |
| `quatra_zone.h`    | Per-zone state machine (`zone_result_t` returned)       |
| `quatra_uart.h`    | JSON formatters + parser for the webapp link            |

> Function-level doc strings (Doxygen-style) live in each header.

---

## API reference

### `quatra_et0.h` — evapotranspiration

```c
float quatra_et0_compute(
    float T_c,           // mean daily air temperature [°C]
    float RH_pct,        // mean daily relative humidity [%]
    float u2_ms,         // wind speed at 2 m [m/s]
    float P_kpa,         // mean daily atmospheric pressure [kPa]
    float Rs_MJm2,       // daily solar radiation [MJ/m²/day]
    float latitude_rad,  // site latitude [rad]
    int   day_of_year);  // [1, 366]
// → ET0 [mm/day], clamped to [0, QUATRA_ET0_MAX_MM_DAY = 20.0]

float quatra_et0_wind_at_2m(float u_h_ms, float h_m);
// Project anemometer wind from height h to FAO-56 reference 2 m
// (log profile). Returns u_h_ms unchanged when h ≤ 0 or h == 2.
```

The integrator computes `Rs_MJm2` from the weather station's `lux`
reading; the conversion constant `QUATRA_LUX_TO_WM2 = 120.0` is exposed
in `quatra_config.h` so client code can share it.

### `quatra_zone.h` — per-zone control

```c
quatra_zone_result_t quatra_zone_update(
    uint8_t              zone_id,    // [0, QUATRA_NUM_ZONES); diagnostics only
    const float          psi[3],     // Watermark readings [cb], already in centibar
    const float          weights[3], // per-depth weights, typically {1, 2, 3}
    float                MAD_cb,     // Management Allowable Depletion [cb]
    float                ETacc_mm,   // current accumulator [mm], integrator-persisted
    float                Q_m3h,      // flow rate [m³/h]
    float                area_m2,    // irrigated area [m²]
    uint32_t             elapsed_s,  // 0 if IDLE, else seconds since irrigation start
    quatra_op_mode_t     mode);      // MANUAL / SEMI_AUTO / FULL_AUTO

float quatra_zone_weighted_psi (const float psi[3], const float weights[3]);
float quatra_zone_accumulate_et(float ETacc_mm, float ETc_mm);
```

Calling convention for `elapsed_s`:

- `elapsed_s == 0` → the zone is currently IDLE (or just transitioned in).
- `elapsed_s  > 0` → the zone is currently IRRIGATING; `elapsed_s` is
  the number of seconds since the irrigation started.

In `QUATRA_MODE_MANUAL` the function returns a safe baseline
(`valve_open=false`, `state=IDLE`); the integrator drives the valve
directly from the `manual_valve` command.

### `quatra_uart.h` — JSON (de)serialization

All formatters return bytes written (including the trailing `'\n'`) or
`-1` on error (truncation, NULL inputs, OOM).

```c
int quatra_uart_format_sensor_data(
        char *buf, size_t len, uint32_t ts_epoch,
        const quatra_weather_inputs_t *w,
        const quatra_zone_result_t results[QUATRA_NUM_ZONES]);

int quatra_uart_format_valve_state(
        char *buf, size_t len, uint32_t ts_epoch,
        uint8_t zone_id, bool open,
        quatra_valve_reason_t reason,
        float D_target_mm,
        float ETacc_mm);

int quatra_uart_format_et_daily(
        char *buf, size_t len, uint32_t ts_epoch,
        float    ET0_mm,
        uint16_t day_of_year,
        const quatra_zone_config_t cfg[QUATRA_NUM_ZONES],
        const float ETc_mm [QUATRA_NUM_ZONES],
        const float ETacc_mm[QUATRA_NUM_ZONES]);

int quatra_uart_format_alarm(
        char *buf, size_t len, uint32_t ts_epoch,
        const char *code,
        int8_t      zone_id,    // -1 to omit from the JSON
        int8_t      sensor_idx, // -1 to omit from the JSON
        const char *detail);    // NULL to omit

int quatra_uart_format_system_status(
        char *buf, size_t len, uint32_t ts_epoch,
        const char *firmware_version,
        bool wifi_connected,
        bool rtc_sync,
        uint32_t uptime_s,
        const quatra_zone_result_t results[QUATRA_NUM_ZONES]);

int quatra_uart_parse_command(const char *json, quatra_uart_command_t *cmd_out);
// 0 on success, -1 on malformed JSON or unknown "type".
```

### Key struct fields (`quatra_types.h`)

`quatra_weather_inputs_t` — exactly what the integrator reads from the
RS-485 weather station:

```c
float temp_c, humidity_pct, wind_ms, pressure_kpa, lux, rain_mm_min;
```

> `Rs_MJm2` is NOT in this struct — the integrator derives it from `lux`.
> `h_anemometer_m` is a global parameter, set via `set_params` command
> (default `2.0`).

`quatra_zone_config_t`:

```c
float kc, mad_cb, weights[3], area_m2, flow_m3h;
```

> There is no `t_max_runtime` field — the safety cap is the global
> `QUATRA_MAX_RUNTIME_S` (`#define` in `quatra_config.h`).

`quatra_zone_result_t` (returned by `zone_update`):

```c
bool                  valve_open;     // act on this
float                 psi[3];         // echoed inputs
float                 psi_avg_cb;     // weighted average
float                 D_applied_mm;   // water applied this session
float                 ETacc_mm;       // updated accumulator — PERSIST
quatra_zone_state_t   state;          // IDLE / IRRIGATING / FAULT
quatra_valve_reason_t reason;         // last transition rationale
```

`quatra_uart_command_t` is a discriminated union; the `type` field
identifies which sub-fields are populated. See the header.

---

## JSON protocol catalog

All frames share the envelope and are `'\n'`-terminated:

```json
{"type": "...", "ts": <epoch_seconds>, "payload": {...}}
```

> The JSON key is `"ts"`, not `"ts_epoch"` (the C parameter name).

### Outbound — ESP32 → CM5 (5 types)

**`sensor_data`** — periodic snapshot:

```json
{
  "type": "sensor_data",
  "ts":   1700000000,
  "payload": {
    "weather": {
      "temp_c":       22.3,
      "humidity_pct": 64.0,
      "wind_ms":      2.1,
      "pressure_kpa": 101.3,
      "lux":          45000,
      "rain_mm_min":  0.0
    },
    "zones": [
      {"id": 0, "psi": [30.0, 45.0, 70.0], "psi_avg": 56.7, "state": "IRRIGATING"},
      … 8 entries total …
    ]
  }
}
```

**`valve_state`** — emitted on transition:

```json
{"type": "valve_state", "ts": ...,
 "payload": {"zone_id": 0, "state": "OPEN", "reason": "MAD_TRIGGER",
             "D_target_mm": 8.5, "ETacc_mm": 4.2}}
```

`reason` ∈ `{MAD_TRIGGER, SOIL_RECOVERED, ET_SATISFIED, MAX_RUNTIME,
MANUAL_CMD, FAULT, NONE}`.

**`et_daily`** — once per day, after the daily ET0 update:

```json
{"type": "et_daily", "ts": ...,
 "payload": {
   "ET0_mm": 5.6, "day_of_year": 152,
   "zones": [{"id": 0, "Kc": 0.85, "ETc_mm": 4.76, "ETacc_mm": 14.28}, … ]
 }}
```

**`alarm`** — fault / warning:

```json
{"type": "alarm", "ts": ...,
 "payload": {"code": "SENSOR_FAULT", "zone_id": 2, "sensor_idx": 1,
             "detail": "Open circuit detected on psi_2 zone 2"}}
```

`zone_id` and `sensor_idx` are omitted from the JSON when the caller
passes `-1`; `detail` is omitted when NULL.

**`system_status`** — heartbeat / on demand:

```json
{"type": "system_status", "ts": ...,
 "payload": {"firmware_version": "1.1.0", "wifi_connected": true,
             "rtc_sync": true, "uptime_s": 12345,
             "active_zones": [0, 3]}}
```

`active_zones` is derived from `results[]` (zones in IRRIGATING state).

### Inbound — CM5 → ESP32 (6 types)

**`set_mode`** — change a zone's operating mode (or all zones with
`zone_id: -1`):

```json
{"type": "set_mode", "ts": ...,
 "payload": {"zone_id": 0, "mode": "MANUAL"}}
```

`mode` ∈ `{MANUAL, SEMI_AUTO, FULL_AUTO}`.

**`manual_valve`** — force a valve (only effective in MANUAL mode):

```json
{"type": "manual_valve", "ts": ...,
 "payload": {"zone_id": 0, "state": "OPEN"}}
```

`state` ∈ `{OPEN, CLOSED}`.

**`set_params`** — bulk update; only present fields are applied:

```json
{"type": "set_params", "ts": ...,
 "payload": {
   "global": {"latitude_deg": 45.0, "h_anemometer_m": 2.0},
   "zones": [
     {"id": 0, "kc": 0.85, "mad_cb": 55.0, "area_m2": 1200.0,
      "flow_m3h": 8.0, "weights": [1.0, 2.0, 3.0]},
     …
   ]
 }}
```

`latitude_deg` is in degrees on the wire; the parser converts to radians.

**`request_status`** — empty payload, asks the device to push a `system_status`:

```json
{"type": "request_status", "ts": ...,
 "payload": {}}
```

**`reset_fault`** — clear FAULT on one zone:

```json
{"type": "reset_fault", "ts": ...,
 "payload": {"zone_id": 2}}
```

**`reset_ETacc`** — reset the daily accumulator for one zone:

```json
{"type": "reset_ETacc", "ts": ...,
 "payload": {"zone_id": 5}}
```

> The type string is **case-sensitive** — capital `E` in `ETacc`.

---

## Default configuration

All defaults are in `include/quatra_config.h`.

| Constant                  | Value         | Notes                                |
| ------------------------- | ------------- | ------------------------------------ |
| `QUATRA_FIRMWARE_VERSION` | `"1.1.0"`     | shown in `system_status` payload     |
| `QUATRA_NUM_ZONES`        | `8`           | max zones                            |
| `QUATRA_SENSORS_PER_ZONE` | `3`           | Watermark per zone                   |
| `QUATRA_PSI_MIN_CB`       | `0.0`         | plausibility lower bound             |
| `QUATRA_PSI_MAX_CB`       | `239.0`       | Watermark 200SS max                  |
| `QUATRA_ET0_MAX_MM_DAY`   | `20.0`        | ET0 clamp                            |
| `QUATRA_MAX_RUNTIME_S`    | `3600`        | safety cap on irrigation [s]         |
| `QUATRA_LATITUDE_RAD`     | `0.7854`      | 45° N default                        |
| `QUATRA_H_ANEMOMETER`     | `2.0`         | FAO reference height [m]             |
| `QUATRA_LUX_TO_WM2`       | `120.0`       | lux → W/m² coefficient               |
| `QUATRA_PARALLEL_ZONES`   | `1`           | concurrent irrigations               |
| `QUATRA_DEFAULT_KC`       | `1.0`         | crop coefficient default             |
| `QUATRA_DEFAULT_MAD_CB`   | `50.0`        | MAD default                          |
| `QUATRA_DEFAULT_WEIGHTS`  | `{1, 2, 3}`   | shallow / mid / deep                 |
| `QUATRA_DEFAULT_AREA_M2`  | `1000.0`      | per zone                             |
| `QUATRA_DEFAULT_FLOW_M3H` | `10.0`        | per zone                             |

Per-zone agronomic parameters (Kc, MAD, area, flow, weights) and the
global latitude / anemometer height are passed in as function arguments,
so they can be reconfigured at runtime via the `set_params` JSON command
without rebuilding the firmware.

---

## Integration responsibilities

These live on your side and are easy to overlook:

1. **Persist `ETacc_mm` across reboots.** It is the daily water-debt
   accumulator; losing it on a power cycle causes the zone to
   over-irrigate (treats the next call as a cold start) or under-irrigate
   (depends on how you reseed it). NVS or any persistent storage is
   fine. Save it whenever `quatra_zone_update` returns.

2. **Reset `ETacc_mm` to zero at local midnight.** The algorithm is
   daily-based. The library does not know the wall clock; you do.

3. **Provide `day_of_year` from a reliable source** (RTC + NTP via CM5).
   The ET0 calculation needs it for solar geometry.

4. **The library is stateless between calls.** Keep any state you need
   (`ETacc_mm`, `elapsed_s`, mode per zone, last config) in your own
   structures and pass them in on every tick.

5. **On `state == FAULT`** the library closes the valve and lets you
   emit the alarm JSON. It is your job to decide when to clear the
   fault: the webapp sends `reset_fault`, you parse it, then call
   `quatra_zone_update` again with healthy inputs.

6. **`Rs_MJm2` is your calculation.** Integrate the lux reading from
   the weather station over the day and convert (see
   `QUATRA_LUX_TO_WM2`).

7. **Buffer sizing for JSON.** `sensor_data` and `et_daily` can reach
   ~1 KB with 8 zones; budget a 1.5 KB stack/heap buffer. All other
   messages fit in 256 B.

8. **Thread safety.** All functions are pure and re-entrant — call from
   any task without locks.

---

## Known limitations

1. **Daily-mean ET0.** The implementation uses the daily-mean approximation
   rather than the FAO-56 Tmin/Tmax variant (which requires a daily min/max
   record). Validated against FAO Example 17 (Bangkok) the deviation is
   ~5% vs the full variant. Acceptable for a prototype.

2. **No dynamic Kc.** Crop coefficient is taken at face value from
   `quatra_zone_config_t.kc`. The integrator chooses how (or whether) to
   schedule it across the season.

3. **`MAX_RUNTIME_S` is a compile-time constant.** Default is 3600 s
   (1 h) per zone. If the LoRa reporting cadence is comparable to or
   longer than this, recompile with a lower value, or promote it to a
   per-zone config field (small patch — see "Optional follow-ups" in the
   release notes).

4. **Lux → W/m² is a single-coefficient approximation.** Adequate for an
   ultrasonic weather station; if calibration data is available later
   it can be refined.

5. **Wind correction assumes a logarithmic profile** (FAO standard,
   neutral atmosphere). For anemometer heights above 10 m it may
   slightly under-correct.

---

## Host-side unit tests

```sh
cd tests
make test
```

The Quatra library is pure C99 with no hardware dependencies, so the
test suite compiles and runs on macOS and Linux. cJSON is vendored
under `tests/vendor/` — no system-wide library install is required.

Current coverage: **35 test suites, ~120 assertions, 0 failures**.
Built with `-Wall -Wextra` under `gcc`/`clang`.

---

## Hardware data path

This library never touches hardware. The integrator's main code does:

```
Watermark sensors → 200SS-VA3/SDI adapter → Dragino LoRaWAN node
                  → LoRaWAN gateway → (network) → CM5 / webapp

Weather station   → RS-485 Modbus → RS-485-UART adapter → ESP32-S3 UART

Relay board       → RS-485 Modbus → RS-485-UART adapter → ESP32-S3 UART

ESP32-S3          ↔ UART ↔ Raspberry CM5 ↔ internet ↔ WebApp
```

All readings reach this library as plain `float` values; all outputs leave
it as plain return values and JSON strings.

---

## Versioning and license

- **Version:** `1.1.1` (declared in `idf_component.yml`).
- **ESP-IDF requirement:** `>= 5.0`. Targets: `esp32`, `esp32s2`,
  `esp32s3`, `esp32c3`, `esp32c6`.
- **License:** proprietary for the library source. The vendored cJSON
  library carries its own MIT license — see `tests/vendor/cJSON.LICENSE`.

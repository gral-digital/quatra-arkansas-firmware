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

1. [At a glance](#at-a-glance)
2. [Step-by-step integration](#step-by-step-integration)
3. [Reference integration example](#reference-integration-example)
4. [Module map](#module-map)
5. [API reference](#api-reference)
   - [`quatra_et0_compute`](#quatra_et0_compute)
   - [`quatra_et0_wind_at_2m`](#quatra_et0_wind_at_2m)
   - [`quatra_zone_update`](#quatra_zone_update)
   - [`quatra_zone_weighted_psi`](#quatra_zone_weighted_psi)
   - [`quatra_zone_accumulate_et`](#quatra_zone_accumulate_et)
   - [`quatra_uart_format_sensor_data`](#quatra_uart_format_sensor_data)
   - [`quatra_uart_format_valve_state`](#quatra_uart_format_valve_state)
   - [`quatra_uart_format_et_daily`](#quatra_uart_format_et_daily)
   - [`quatra_uart_format_alarm`](#quatra_uart_format_alarm)
   - [`quatra_uart_format_system_status`](#quatra_uart_format_system_status)
   - [`quatra_uart_parse_command`](#quatra_uart_parse_command)
   - [Enum-to-string helpers](#enum-to-string-helpers)
6. [Data types](#data-types)
7. [JSON protocol catalog](#json-protocol-catalog)
8. [Default configuration](#default-configuration)
9. [Integration responsibilities](#integration-responsibilities)
10. [Known limitations](#known-limitations)
11. [Host-side unit tests](#host-side-unit-tests)
12. [Hardware data path](#hardware-data-path)
13. [Versioning and license](#versioning-and-license)

---

## At a glance

```c
quatra_zone_result_t r = quatra_zone_update(
    zone_id, psi, weights, mad_cb, etacc_mm,
    flow_m3h, area_m2, elapsed_s, mode);

if (r.valve_open) drive_relay(zone_id, true);
else              drive_relay(zone_id, false);

save_etacc_to_nvs(zone_id, r.ETacc_mm);

char buf[1024];
int n = quatra_uart_format_valve_state(
    buf, sizeof buf, epoch_now,
    zone_id, r.valve_open, r.reason,
    r.D_applied_mm, r.ETacc_mm);
if (n > 0) uart_write_bytes(YOUR_UART, buf, n);
```

The library is pure: no globals, no statics, no allocation after init.
You can call it from multiple tasks in parallel without locks.

For a full working skeleton see
[`examples/main.c`](../../examples/main.c) and the next section.

---

## Step-by-step integration

A typical ESP-IDF integration follows these twelve steps. The reference
example at [`examples/main.c`](../../examples/main.c) implements every
one of them, with `TODO` markers on the hardware functions you provide.

### Step 1 — Place the component

Copy `components/quatra/` into your project's `components/` directory.
Your tree looks like:

```
my-project/
├── CMakeLists.txt
├── main/
│   └── CMakeLists.txt
└── components/
    └── quatra/        ← this library
```

### Step 2 — Wire the build

In `main/CMakeLists.txt`, add `quatra` to your component's `REQUIRES`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES quatra
)
```

### Step 3 — Include the headers

```c
#include "quatra_config.h"
#include "quatra_et0.h"
#include "quatra_types.h"
#include "quatra_uart.h"
#include "quatra_zone.h"
```

### Step 4 — Define the per-zone state YOU own

The library is stateless; you keep state between ticks:

```c
typedef struct {
    quatra_zone_config_t cfg;     /* kc, MAD, area, flow, weights */
    quatra_op_mode_t     mode;
    float                ETacc_mm;        /* persisted in NVS */
    bool                 irrigating;
    uint32_t             session_start_s;
} zone_state_t;

static zone_state_t zones[QUATRA_NUM_ZONES];
```

Initialize each zone from defaults (see `quatra_config.h`) or from your
saved configuration, and load `ETacc_mm` from NVS on boot.

### Step 5 — Read inputs from hardware

Once per tick, gather the inputs the library needs. The library does
not read hardware — you do:

```c
float psi[3];
hw_read_zone_psi(z, psi);                 /* LoRa → CM5 → ESP32 */

quatra_weather_inputs_t w;
hw_read_weather(&w);                      /* RS-485 station    */
```

### Step 6 — Call the zone update

```c
uint32_t elapsed_s = zones[z].irrigating
                   ? (now_s - zones[z].session_start_s)
                   : 0;

quatra_zone_result_t r = quatra_zone_update(
    z, psi, zones[z].cfg.weights,
    zones[z].cfg.mad_cb, zones[z].ETacc_mm,
    zones[z].cfg.flow_m3h, zones[z].cfg.area_m2,
    elapsed_s, zones[z].mode);
```

### Step 7 — Drive the valve

```c
hw_set_relay(z, r.valve_open);
```

Update your session-tracking state to feed `elapsed_s` correctly on
the next tick (start a timer when the valve opens, stop it when it
closes).

### Step 8 — Persist the accumulator

```c
zones[z].ETacc_mm = r.ETacc_mm;
nvs_save_etacc(z, r.ETacc_mm);
```

### Step 9 — Emit telemetry events

On valve transitions (the result's `valve_open` differs from the
previous tick) format and send `valve_state`:

```c
char buf[256];
int n = quatra_uart_format_valve_state(
    buf, sizeof buf, now_s,
    z, r.valve_open, r.reason,
    r.D_applied_mm, r.ETacc_mm);
if (n > 0) uart_send(buf, n);
```

On `state == QUATRA_ZONE_FAULT` send an `alarm`:

```c
char buf[256];
int n = quatra_uart_format_alarm(
    buf, sizeof buf, now_s,
    "SENSOR_FAULT", (int8_t)z, -1,
    "two or more sensors out of range");
if (n > 0) uart_send(buf, n);
```

Push a `sensor_data` snapshot periodically (~30 s) with all zones and
the latest weather.

### Step 10 — Daily ET0 task

Once a day at local midnight, compute ET0 and per-zone ETc, accumulate,
and emit `et_daily`:

```c
float u2 = quatra_et0_wind_at_2m(w.wind_ms, h_anemometer_m);
float ET0 = quatra_et0_compute(
    w.temp_c, w.humidity_pct, u2,
    w.pressure_kpa, Rs_MJm2,
    site_latitude_rad, day_of_year);

for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z) {
    float ETc = zones[z].cfg.kc * ET0;
    zones[z].ETacc_mm = quatra_zone_accumulate_et(zones[z].ETacc_mm, ETc);
    nvs_save_etacc(z, zones[z].ETacc_mm);
}
/* … then format and send the et_daily JSON message … */
```

`Rs_MJm2` is your integration of `lux` over the day — use
`QUATRA_LUX_TO_WM2 = 120.0` for the per-sample conversion.

### Step 11 — UART RX: parse and apply commands

On every line received over UART:

```c
quatra_uart_command_t cmd;
if (quatra_uart_parse_command(line, &cmd) == 0) {
    switch (cmd.type) {
        case QUATRA_CMD_SET_MODE:      /* … */ break;
        case QUATRA_CMD_MANUAL_VALVE:  /* … */ break;
        case QUATRA_CMD_SET_PARAMS:    /* … */ break;
        case QUATRA_CMD_REQUEST_STATUS:/* respond with system_status */ break;
        case QUATRA_CMD_RESET_FAULT:   /* clear your local fault latch */ break;
        case QUATRA_CMD_RESET_ETACC:   /* zero ETacc for the zone */    break;
        default: break;
    }
}
```

### Step 12 — Wire the tasks

Create three FreeRTOS tasks:

| Task                | Period         | Purpose                                      |
| ------------------- | -------------- | -------------------------------------------- |
| `irrigation_task`   | 1 Hz           | per-zone tick + periodic `sensor_data`       |
| `daily_et0_task`    | once / day     | ET0 + ETc + `et_daily`                       |
| `uart_rx_task`      | blocking RX    | parse commands and apply them                |

Reasonable stack sizes: 8 KB for `irrigation_task`, 4 KB for the others.
Priorities depend on your system; UART RX should be at least as high
as the irrigation task so commands are not delayed.

---

## Reference integration example

[`examples/main.c`](../../examples/main.c) implements every step above:
configuration defaults, the 1 Hz control tick, the 30 s telemetry push,
edge-triggered `valve_state` events, FAULT alarms, the daily ET0 task,
and the UART RX dispatcher.

It is intentionally not a turnkey project — copy it into your project's
`main/` directory and fill in the eleven `TODO` stubs at the top (RS-485
read, LoRa read, relay driver, RTC, NVS, UART transport).

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

Every public function is documented below with parameters, return value
and notes. Header file paths are relative to `components/quatra/include/`.

---

### `quatra_et0_compute`

Compute daily reference evapotranspiration ET0 using the FAO-56
Penman-Monteith equation (steps 2 → 13 of PRD §5.2). Pure math, no
hardware.

**Header:** `quatra_et0.h`

```c
float quatra_et0_compute(float T_c,
                         float RH_pct,
                         float u2_ms,
                         float P_kpa,
                         float Rs_MJm2,
                         float latitude_rad,
                         int   day_of_year);
```

**Parameters**
- `T_c` — mean daily air temperature, °C.
- `RH_pct` — mean daily relative humidity, %.
- `u2_ms` — wind speed at 2 m height, m/s. Use
  [`quatra_et0_wind_at_2m`](#quatra_et0_wind_at_2m) if the anemometer is
  not at 2 m.
- `P_kpa` — mean daily atmospheric pressure, kPa.
- `Rs_MJm2` — integrated daily solar radiation, MJ/m²/day. The
  integrator derives this from the weather station's `lux` channel —
  see `QUATRA_LUX_TO_WM2` in `quatra_config.h`.
- `latitude_rad` — site latitude, radians (positive north).
- `day_of_year` — day of year in `[1, 366]`.

**Returns**
ET0 in mm/day, clamped to `[0, QUATRA_ET0_MAX_MM_DAY]` (20.0 by default).
Invalid or out-of-range inputs are clamped/sanitised internally.

**Notes**
- The function is pure and re-entrant.
- Call once per day, typically just after midnight in the same task
  that emits the `et_daily` JSON.

---

### `quatra_et0_wind_at_2m`

Project a wind reading from anemometer height to the FAO-56 reference
height of 2 m, using the standard log profile.

**Header:** `quatra_et0.h`

```c
float quatra_et0_wind_at_2m(float u_h_ms, float h_m);
```

**Parameters**
- `u_h_ms` — wind speed at the anemometer mounting height, m/s.
- `h_m` — anemometer mounting height, m.

**Returns**
Wind speed at 2 m, m/s. If `h_m == 2.0` the input is returned
unchanged. Invalid heights (`h_m <= 0`) also return the input
unchanged.

---

### `quatra_zone_update`

Run one tick of the per-zone state machine. Pure function: given the
current inputs it returns the action the integrator must take next
(valve state, new state, updated accumulator). Implements PRD §4.2
(states) and §5.5 (stop conditions).

**Header:** `quatra_zone.h`

```c
quatra_zone_result_t quatra_zone_update(
    uint8_t              zone_id,
    const float          psi[QUATRA_SENSORS_PER_ZONE],
    const float          weights[QUATRA_SENSORS_PER_ZONE],
    float                MAD_cb,
    float                ETacc_mm,
    float                Q_m3h,
    float                area_m2,
    uint32_t             elapsed_s,
    quatra_op_mode_t     mode);
```

**Parameters**
- `zone_id` — zone index in `[0, QUATRA_NUM_ZONES)`. Used for
  diagnostics only.
- `psi` — array of 3 Watermark readings, centibar, already converted
  by the upstream adapter.
- `weights` — per-depth weights for the average (typically `{1, 2, 3}`).
- `MAD_cb` — Management Allowable Depletion threshold, centibar.
- `ETacc_mm` — current accumulated ET, mm. The integrator persists this
  across reboots and resets it at local midnight.
- `Q_m3h` — flow rate for the zone, m³/h.
- `area_m2` — irrigated area, m².
- `elapsed_s` — `0` if the zone is IDLE, otherwise the number of
  seconds elapsed since the irrigation session started.
- `mode` — operating mode: `QUATRA_MODE_MANUAL`, `QUATRA_MODE_SEMI_AUTO`
  or `QUATRA_MODE_FULL_AUTO`.

**Returns**
A populated `quatra_zone_result_t` (see [Data types](#data-types)).
The integrator should:
- Drive the relay from `r.valve_open`.
- Persist `r.ETacc_mm` to NVS.
- Format and send a `valve_state` JSON event when `r.valve_open` flips.
- Format and send an `alarm` when `r.state == QUATRA_ZONE_FAULT`.

**Notes**
- In `QUATRA_MODE_MANUAL` the function returns a safe baseline
  (`valve_open=false`, `state=IDLE`) — the integrator drives the valve
  directly via the inbound `manual_valve` command.
- If 2 or more sensors are out of plausibility range `[PSI_MIN_CB,
  PSI_MAX_CB]`, the zone enters `QUATRA_ZONE_FAULT` and the valve is
  closed. A single out-of-range sensor is silently excluded from the
  weighted average; the zone continues operating on the two healthy
  sensors.

---

### `quatra_zone_weighted_psi`

Compute the weighted root-zone tension average used by
`quatra_zone_update`. Exposed as a helper so the integrator can use the
same value in their own telemetry.

**Header:** `quatra_zone.h`

```c
float quatra_zone_weighted_psi(
    const float psi[QUATRA_SENSORS_PER_ZONE],
    const float weights[QUATRA_SENSORS_PER_ZONE]);
```

**Parameters**
- `psi` — array of 3 readings, centibar.
- `weights` — array of 3 weights. If all are zero or invalid, the
  function falls back to an equal-weight average over the valid
  sensors.

**Returns**
The weighted average in centibar. Out-of-range readings (NaN, negative,
> `PSI_MAX_CB`) are excluded from the sum. If no readings are valid the
function returns `0.0`.

---

### `quatra_zone_accumulate_et`

Apply one daily ETc contribution to the running accumulator.

**Header:** `quatra_zone.h`

```c
float quatra_zone_accumulate_et(float ETacc_mm, float ETc_mm);
```

**Parameters**
- `ETacc_mm` — current accumulator value, mm.
- `ETc_mm` — daily crop evapotranspiration, mm. `ETc = Kc * ET0`.

**Returns**
`ETacc_mm + max(0, ETc_mm)`. NaN / negative `ETc` is ignored. NaN /
negative `ETacc_mm` is reset to the `ETc` contribution.

---

### `quatra_uart_format_sensor_data`

Format a `sensor_data` JSON frame: latest weather snapshot plus per-zone
psi readings.

**Header:** `quatra_uart.h`

```c
int quatra_uart_format_sensor_data(
    char *buf, size_t buf_len,
    uint32_t ts_epoch,
    const quatra_weather_inputs_t *w,
    const quatra_zone_result_t results[QUATRA_NUM_ZONES]);
```

**Parameters**
- `buf` / `buf_len` — destination buffer and capacity. ~1 KB is
  sufficient for 8 zones.
- `ts_epoch` — frame timestamp, epoch seconds. The JSON key is `"ts"`.
- `w` — latest weather inputs (caller-filled).
- `results` — per-zone latest result snapshots.

**Returns**
Bytes written (including the trailing `'\n'`), or `-1` on truncation /
NULL inputs / OOM.

---

### `quatra_uart_format_valve_state`

Format a `valve_state` JSON frame for one zone, on transition.

**Header:** `quatra_uart.h`

```c
int quatra_uart_format_valve_state(
    char *buf, size_t buf_len,
    uint32_t ts_epoch,
    uint8_t  zone_id,
    bool     open,
    quatra_valve_reason_t reason,
    float    D_target_mm,
    float    ETacc_mm);
```

**Parameters**
- `buf` / `buf_len` — destination buffer (256 B is sufficient).
- `ts_epoch` — frame timestamp.
- `zone_id` — zone that transitioned, `[0, QUATRA_NUM_ZONES)`.
- `open` — new valve state.
- `reason` — `quatra_valve_reason_t` describing why the transition
  happened. Stringified in the JSON.
- `D_target_mm` — target water depth for the session, mm.
- `ETacc_mm` — current accumulator value, mm.

**Returns**
Bytes written, or `-1` on error.

---

### `quatra_uart_format_et_daily`

Format the once-per-day `et_daily` frame: global ET0 plus per-zone Kc,
ETc and updated ETacc.

**Header:** `quatra_uart.h`

```c
int quatra_uart_format_et_daily(
    char *buf, size_t buf_len,
    uint32_t ts_epoch,
    float    ET0_mm,
    uint16_t day_of_year,
    const quatra_zone_config_t cfg[QUATRA_NUM_ZONES],
    const float ETc_mm [QUATRA_NUM_ZONES],
    const float ETacc_mm[QUATRA_NUM_ZONES]);
```

**Parameters**
- `buf` / `buf_len` — destination buffer (~1 KB).
- `ts_epoch` — frame timestamp.
- `ET0_mm` — ET0 for the day, mm/day.
- `day_of_year` — day of year `[1, 366]`.
- `cfg` — per-zone config (only `kc` is read by the formatter, but the
  full struct is taken for forward compatibility).
- `ETc_mm` — per-zone daily ETc.
- `ETacc_mm` — per-zone updated accumulator.

**Returns**
Bytes written, or `-1` on error.

---

### `quatra_uart_format_alarm`

Format an `alarm` JSON frame.

**Header:** `quatra_uart.h`

```c
int quatra_uart_format_alarm(
    char *buf, size_t buf_len,
    uint32_t ts_epoch,
    const char *code,
    int8_t      zone_id,
    int8_t      sensor_idx,
    const char *detail);
```

**Parameters**
- `buf` / `buf_len` — destination buffer (256 B suffices).
- `ts_epoch` — frame timestamp.
- `code` — short alarm code string, e.g. `"SENSOR_FAULT"`,
  `"ET0_OUT_OF_RANGE"`. Must be non-NULL.
- `zone_id` — affected zone, or `-1` to omit the field from the JSON.
- `sensor_idx` — affected sensor in the zone, or `-1` to omit.
- `detail` — free-form description, or `NULL` to omit.

**Returns**
Bytes written, or `-1` on error.

---

### `quatra_uart_format_system_status`

Format a `system_status` JSON frame: firmware version, link state,
uptime, list of currently-irrigating zones.

**Header:** `quatra_uart.h`

```c
int quatra_uart_format_system_status(
    char *buf, size_t buf_len,
    uint32_t ts_epoch,
    const char *firmware_version,
    bool wifi_connected,
    bool rtc_sync,
    uint32_t uptime_s,
    const quatra_zone_result_t results[QUATRA_NUM_ZONES]);
```

**Parameters**
- `buf` / `buf_len` — destination buffer (512 B suffices).
- `ts_epoch` — frame timestamp.
- `firmware_version` — version string. Pass `QUATRA_FIRMWARE_VERSION`
  from `quatra_config.h` to stay in sync with the library.
- `wifi_connected` — link-up boolean reported by your network stack.
- `rtc_sync` — true when the RTC is synced with NTP.
- `uptime_s` — seconds since boot.
- `results` — per-zone latest result. The formatter derives the
  `"active_zones"` list automatically from those in
  `QUATRA_ZONE_IRRIGATING` state.

**Returns**
Bytes written, or `-1` on error.

---

### `quatra_uart_parse_command`

Parse one inbound JSON frame into a typed `quatra_uart_command_t`.

**Header:** `quatra_uart.h`

```c
int quatra_uart_parse_command(const char *json, quatra_uart_command_t *cmd_out);
```

**Parameters**
- `json` — NUL-terminated JSON frame. The caller has already stripped
  the trailing `'\n'` (or it's safely ignored by the JSON parser).
- `cmd_out` — populated discriminated-union command struct. Only the
  fields relevant to `cmd_out->type` are written; everything else is
  zero-initialised.

**Returns**
`0` on success, `-1` on malformed JSON or unknown `"type"`.

**Recognised `"type"` values:** `"set_params"`, `"set_mode"`,
`"manual_valve"`, `"request_status"`, `"reset_fault"`, `"reset_ETacc"`.

---

### Enum-to-string helpers

Useful when you want to stringify enum values in your own logs:

```c
const char *quatra_uart_state_str (quatra_zone_state_t s);
const char *quatra_uart_reason_str(quatra_valve_reason_t r);
const char *quatra_uart_mode_str  (quatra_op_mode_t m);
```

All three return a pointer to a string literal — never NULL, never
needs to be freed.

---

## Data types

### `quatra_weather_inputs_t` — `quatra_types.h`

What the integrator reads from the RS-485 weather station and passes
to the formatters and (via ET0) to the math:

```c
typedef struct {
    float temp_c;          // [°C]
    float humidity_pct;    // [%]
    float wind_ms;         // [m/s]
    float pressure_kpa;    // [kPa]
    float lux;             // raw lux
    float rain_mm_min;     // [mm/min]
} quatra_weather_inputs_t;
```

> `Rs_MJm2` is NOT in this struct — derive it from `lux` over the day
> using `QUATRA_LUX_TO_WM2 = 120.0`.
>
> `h_anemometer_m` is NOT here either — it is a global parameter set
> via the `set_params` command (default `QUATRA_H_ANEMOMETER = 2.0`).

### `quatra_zone_config_t` — `quatra_types.h`

```c
typedef struct {
    float kc;                                   // crop coefficient
    float mad_cb;                               // MAD threshold [cb]
    float weights[QUATRA_SENSORS_PER_ZONE];     // per-depth weights
    float area_m2;                              // irrigated area [m²]
    float flow_m3h;                             // flow rate [m³/h]
} quatra_zone_config_t;
```

> No `t_max_runtime` field — the safety cap is the global
> `QUATRA_MAX_RUNTIME_S = 3600` (compile-time `#define`).

### `quatra_zone_result_t` — `quatra_types.h`

Returned by `quatra_zone_update`:

```c
typedef struct {
    bool                  valve_open;     // act on this
    float                 psi[3];         // echoed inputs
    float                 psi_avg_cb;     // weighted average
    float                 D_applied_mm;   // water applied this session
    float                 ETacc_mm;       // updated accumulator (persist)
    quatra_zone_state_t   state;          // IDLE / IRRIGATING / FAULT
    quatra_valve_reason_t reason;         // last transition rationale
} quatra_zone_result_t;
```

### `quatra_uart_command_t` — `quatra_types.h`

Discriminated union populated by `quatra_uart_parse_command`. The
`type` field is one of `quatra_cmd_type_t`:

| `type`                      | Populated fields                                                                 |
| --------------------------- | -------------------------------------------------------------------------------- |
| `QUATRA_CMD_SET_MODE`       | `mode_zone_id` (int8_t, -1 = all), `mode_value`                                  |
| `QUATRA_CMD_MANUAL_VALVE`   | `manual_zone_id`, `manual_open`                                                  |
| `QUATRA_CMD_SET_PARAMS`     | `setp_mask`, `setp_latitude_rad`, `setp_h_anemometer_m`, `setp_zone_mask`, `setp_zones[8]` |
| `QUATRA_CMD_REQUEST_STATUS` | (none — empty payload on the wire)                                               |
| `QUATRA_CMD_RESET_FAULT`    | `target_zone_id`                                                                 |
| `QUATRA_CMD_RESET_ETACC`    | `target_zone_id`                                                                 |

### Enums — `quatra_types.h`

```c
typedef enum {
    QUATRA_ZONE_IDLE = 0, QUATRA_ZONE_IRRIGATING, QUATRA_ZONE_FAULT,
} quatra_zone_state_t;

typedef enum {
    QUATRA_MODE_MANUAL = 0, QUATRA_MODE_SEMI_AUTO, QUATRA_MODE_FULL_AUTO,
} quatra_op_mode_t;

typedef enum {
    QUATRA_REASON_NONE = 0,
    QUATRA_REASON_MAD_TRIGGER,
    QUATRA_REASON_SOIL_RECOVERED,
    QUATRA_REASON_ET_SATISFIED,
    QUATRA_REASON_MAX_RUNTIME,
    QUATRA_REASON_FAULT,
    QUATRA_REASON_MANUAL_CMD,
} quatra_valve_reason_t;
```

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

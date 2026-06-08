/**
 * @file    examples/main.c
 * @brief   Quatra Arkansas — reference integration for ESP-IDF.
 *
 * This file is ILLUSTRATIVE — not a turnkey project. Copy it into your
 * ESP-IDF project's `main/` directory, fill in the hardware stub functions
 * marked with `TODO`, and adapt task priorities/stack sizes to your system.
 *
 * Everything Quatra needs from you is wired here:
 *
 *   - One-time per-zone configuration (defaults from quatra_config.h).
 *   - 1 Hz control tick that calls quatra_zone_update() for every zone,
 *     drives the relay, persists ETacc, and emits valve_state / alarm
 *     JSON messages on transitions.
 *   - 30 s telemetry push of sensor_data.
 *   - Once-per-day task that calls quatra_et0_compute(), accumulates per-zone
 *     ETc, persists ETacc, and emits et_daily.
 *   - UART RX task that parses inbound commands and applies them
 *     (set_mode, manual_valve, set_params, request_status,
 *      reset_fault, reset_ETacc).
 *
 * All side-effects (NVS, GPIO, RS-485, LoRa, RTC, UART) are hidden behind
 * single-line stub functions at the top — those are YOUR code.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "quatra_config.h"
#include "quatra_et0.h"
#include "quatra_types.h"
#include "quatra_uart.h"
#include "quatra_zone.h"

/* ========================================================================= *
 * 1. Hardware stubs — IMPLEMENT THESE IN YOUR PROJECT                       *
 * ========================================================================= */

/* Declare these in a header of your own (e.g. `app_hw.h`) and define them
 * in a separate translation unit. They are NOT `static` so the linker can
 * find your implementations. */

/** Initialize your peripherals (UART, RS-485, GPIO relays, …). */
void     hw_init(void);                                                     /* TODO */

/** Latest weather snapshot read from the RS-485 station. */
void     hw_read_weather(quatra_weather_inputs_t *out);                     /* TODO RS-485 */

/** Latest soil tension readings for one zone, in centibar. */
void     hw_read_zone_psi(uint8_t zone, float psi_out[QUATRA_SENSORS_PER_ZONE]); /* TODO LoRa */

/** Drive the per-zone solenoid relay. */
void     hw_set_relay(uint8_t zone, bool open);                             /* TODO RS-485 relay */

/** Wall-clock seconds since epoch (RTC + NTP). */
uint32_t hw_epoch_now(void);                                                /* TODO RTC */

/** Day of year [1, 366] from RTC. */
int      hw_day_of_year(void);                                              /* TODO RTC */

/** Persist / load the daily ET accumulator across reboots. */
void     nvs_save_etacc(uint8_t zone, float etacc);                         /* TODO NVS */
float    nvs_load_etacc(uint8_t zone);                                      /* TODO NVS */

/** UART helpers — your transport, your blocking semantics. */
int      uart_send     (const char *buf, int n);                            /* TODO UART TX */
int      uart_recv_line(char *buf, int max_n);                              /* TODO UART RX (blocks on '\n') */

/* ========================================================================= *
 * 2. Integration-owned per-zone state                                       *
 * ========================================================================= */

typedef struct {
    quatra_zone_config_t cfg;
    quatra_op_mode_t     mode;
    float                ETacc_mm;        /**< Persisted to NVS.            */
    bool                 irrigating;      /**< True between start and stop. */
    uint32_t             session_start_s; /**< For elapsed_s computation.   */
    bool                 last_valve_open; /**< For edge-triggered events.   */
} zone_state_t;

static zone_state_t s_zones[QUATRA_NUM_ZONES];
static float        s_site_latitude_rad = QUATRA_LATITUDE_RAD;
static float        s_h_anemometer_m    = QUATRA_H_ANEMOMETER;
static uint32_t     s_boot_epoch_s      = 0;

/* ========================================================================= *
 * 3. One-time setup                                                         *
 * ========================================================================= */

static void zones_init_defaults(void)
{
    const float default_weights[QUATRA_SENSORS_PER_ZONE] = QUATRA_DEFAULT_WEIGHTS;
    for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z) {
        s_zones[z].cfg.kc       = QUATRA_DEFAULT_KC;
        s_zones[z].cfg.mad_cb   = QUATRA_DEFAULT_MAD_CB;
        s_zones[z].cfg.area_m2  = QUATRA_DEFAULT_AREA_M2;
        s_zones[z].cfg.flow_m3h = QUATRA_DEFAULT_FLOW_M3H;
        memcpy(s_zones[z].cfg.weights, default_weights, sizeof default_weights);
        s_zones[z].mode            = QUATRA_DEFAULT_MODE;
        s_zones[z].ETacc_mm        = nvs_load_etacc(z);
        s_zones[z].irrigating      = false;
        s_zones[z].session_start_s = 0;
        s_zones[z].last_valve_open = false;
    }
}

/* ========================================================================= *
 * 4. Irrigation control task — 1 Hz                                         *
 * ========================================================================= */

static void zone_tick(uint8_t z, uint32_t now_s, quatra_zone_result_t *out_snap)
{
    float psi[QUATRA_SENSORS_PER_ZONE];
    hw_read_zone_psi(z, psi);

    const uint32_t elapsed_s = s_zones[z].irrigating
                              ? (now_s - s_zones[z].session_start_s)
                              : 0;

    quatra_zone_result_t r = quatra_zone_update(
        z, psi, s_zones[z].cfg.weights,
        s_zones[z].cfg.mad_cb, s_zones[z].ETacc_mm,
        s_zones[z].cfg.flow_m3h, s_zones[z].cfg.area_m2,
        elapsed_s, s_zones[z].mode);

    /* Drive the valve. */
    hw_set_relay(z, r.valve_open);

    /* Update session-tracking state. */
    if (r.valve_open && !s_zones[z].irrigating) {
        s_zones[z].irrigating      = true;
        s_zones[z].session_start_s = now_s;
    } else if (!r.valve_open && s_zones[z].irrigating) {
        s_zones[z].irrigating = false;
    }

    /* Persist the daily accumulator. */
    s_zones[z].ETacc_mm = r.ETacc_mm;
    nvs_save_etacc(z, r.ETacc_mm);

    /* Emit a valve_state event on edges. */
    if (s_zones[z].last_valve_open != r.valve_open) {
        char buf[256];
        int n = quatra_uart_format_valve_state(
            buf, sizeof buf, now_s,
            z, r.valve_open, r.reason,
            r.D_applied_mm, r.ETacc_mm);
        if (n > 0) uart_send(buf, n);
        s_zones[z].last_valve_open = r.valve_open;
    }

    /* On FAULT, emit an alarm. */
    if (r.state == QUATRA_ZONE_FAULT) {
        char buf[256];
        int n = quatra_uart_format_alarm(
            buf, sizeof buf, now_s,
            "SENSOR_FAULT", (int8_t)z, -1,
            "two or more sensors out of plausibility range");
        if (n > 0) uart_send(buf, n);
    }

    if (out_snap) *out_snap = r;
}

static void irrigation_task(void *arg)
{
    (void)arg;
    quatra_zone_result_t  snap[QUATRA_NUM_ZONES] = {0};
    uint32_t              last_sensor_push       = 0;

    for (;;) {
        const uint32_t now_s = hw_epoch_now();

        for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z) {
            zone_tick(z, now_s, &snap[z]);
        }

        /* Periodic sensor_data telemetry. */
        if (now_s - last_sensor_push >= 30) {
            quatra_weather_inputs_t w;
            hw_read_weather(&w);

            char buf[1536];
            int n = quatra_uart_format_sensor_data(buf, sizeof buf, now_s, &w, snap);
            if (n > 0) uart_send(buf, n);
            last_sensor_push = now_s;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ========================================================================= *
 * 5. Daily ET0 task — runs once a day at local midnight                     *
 * ========================================================================= */

static void daily_et0_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* TODO: sleep until next local midnight; here we use a fixed 24 h. */
        vTaskDelay(pdMS_TO_TICKS(24 * 60 * 60 * 1000));

        quatra_weather_inputs_t w;
        hw_read_weather(&w);

        /* Project anemometer wind to FAO-56 reference height of 2 m. */
        const float u2 = quatra_et0_wind_at_2m(w.wind_ms, s_h_anemometer_m);

        /* TODO: integrate lux over the day → Rs in MJ/m²/day. Use
         *       QUATRA_LUX_TO_WM2 for the per-sample conversion. */
        const float Rs_MJm2 = 18.0f;   /* placeholder */

        const float ET0 = quatra_et0_compute(
            w.temp_c, w.humidity_pct, u2,
            w.pressure_kpa, Rs_MJm2,
            s_site_latitude_rad, hw_day_of_year());

        /* Update per-zone ETc and accumulators. */
        float ETc_arr  [QUATRA_NUM_ZONES];
        float ETacc_arr[QUATRA_NUM_ZONES];
        quatra_zone_config_t cfgs[QUATRA_NUM_ZONES];
        for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z) {
            ETc_arr[z]          = s_zones[z].cfg.kc * ET0;
            s_zones[z].ETacc_mm = quatra_zone_accumulate_et(s_zones[z].ETacc_mm, ETc_arr[z]);
            ETacc_arr[z]        = s_zones[z].ETacc_mm;
            cfgs[z]             = s_zones[z].cfg;
            nvs_save_etacc(z, s_zones[z].ETacc_mm);
        }

        /* Emit et_daily. */
        char buf[1536];
        int n = quatra_uart_format_et_daily(
            buf, sizeof buf, hw_epoch_now(),
            ET0, (uint16_t)hw_day_of_year(),
            cfgs, ETc_arr, ETacc_arr);
        if (n > 0) uart_send(buf, n);
    }
}

/* ========================================================================= *
 * 6. UART RX task — parse and apply inbound commands                        *
 * ========================================================================= */

static void apply_command(const quatra_uart_command_t *cmd)
{
    switch (cmd->type) {
        case QUATRA_CMD_SET_MODE:
            if (cmd->mode_zone_id < 0) {
                for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z)
                    s_zones[z].mode = cmd->mode_value;
            } else if (cmd->mode_zone_id < QUATRA_NUM_ZONES) {
                s_zones[cmd->mode_zone_id].mode = cmd->mode_value;
            }
            break;

        case QUATRA_CMD_MANUAL_VALVE:
            if (cmd->manual_zone_id < QUATRA_NUM_ZONES &&
                s_zones[cmd->manual_zone_id].mode == QUATRA_MODE_MANUAL) {
                hw_set_relay(cmd->manual_zone_id, cmd->manual_open);
            }
            break;

        case QUATRA_CMD_SET_PARAMS:
            if (cmd->setp_mask & QUATRA_SETPARAM_MASK_LATITUDE)
                s_site_latitude_rad = cmd->setp_latitude_rad;
            if (cmd->setp_mask & QUATRA_SETPARAM_MASK_HANEM)
                s_h_anemometer_m = cmd->setp_h_anemometer_m;
            for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z) {
                if (cmd->setp_zone_mask & (1U << z))
                    s_zones[z].cfg = cmd->setp_zones[z];
            }
            break;

        case QUATRA_CMD_REQUEST_STATUS: {
            quatra_zone_result_t snap[QUATRA_NUM_ZONES] = {0};
            for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z) {
                snap[z].state = s_zones[z].irrigating
                              ? QUATRA_ZONE_IRRIGATING
                              : QUATRA_ZONE_IDLE;
            }
            char buf[512];
            int n = quatra_uart_format_system_status(
                buf, sizeof buf, hw_epoch_now(),
                QUATRA_FIRMWARE_VERSION,
                /* wifi_connected = */ true,
                /* rtc_sync       = */ true,
                /* uptime_s       = */ hw_epoch_now() - s_boot_epoch_s,
                snap);
            if (n > 0) uart_send(buf, n);
            break;
        }

        case QUATRA_CMD_RESET_FAULT:
            /* The library re-evaluates state on every tick. Once the
             * underlying psi readings come back into range, the zone
             * automatically exits FAULT. No persistent fault latch. */
            break;

        case QUATRA_CMD_RESET_ETACC:
            if (cmd->target_zone_id < QUATRA_NUM_ZONES) {
                s_zones[cmd->target_zone_id].ETacc_mm = 0.0f;
                nvs_save_etacc(cmd->target_zone_id, 0.0f);
            }
            break;

        default:
            break;
    }
}

static void uart_rx_task(void *arg)
{
    (void)arg;
    char line[1024];
    for (;;) {
        int n = uart_recv_line(line, sizeof line);  /* blocks on '\n' */
        if (n <= 0) continue;
        quatra_uart_command_t cmd;
        if (quatra_uart_parse_command(line, &cmd) == 0) {
            apply_command(&cmd);
        }
    }
}

/* ========================================================================= *
 * 7. app_main                                                               *
 * ========================================================================= */

void app_main(void)
{
    nvs_flash_init();
    hw_init();
    zones_init_defaults();
    s_boot_epoch_s = hw_epoch_now();

    xTaskCreate(irrigation_task, "quatra_tick",    8192, NULL, 5, NULL);
    xTaskCreate(daily_et0_task,  "quatra_daily",   4096, NULL, 4, NULL);
    xTaskCreate(uart_rx_task,    "quatra_uart_rx", 4096, NULL, 6, NULL);
}

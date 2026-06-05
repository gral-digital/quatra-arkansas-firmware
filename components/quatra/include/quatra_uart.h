/**
 * @file    quatra_uart.h
 * @brief   JSON payload (de)serialization for the webapp link.
 *
 * Per PRD v1.1 amendments §2 this module performs ONLY string operations:
 * it fills a caller-provided char buffer with a UTF-8 JSON frame (and a
 * trailing newline), or parses an inbound JSON frame into a typed command.
 * It does NOT touch the UART driver — the integrator writes/reads bytes.
 *
 * Wire format (PRD §8.2):
 *   { "type": "...", "ts": <epoch_s>, "payload": { ... } } \n
 *
 * Depends on cJSON. Inside ESP-IDF cJSON is provided by the bundled `json`
 * component; on host builds it is available via `apt install libcjson-dev`
 * or `brew install cjson`.
 */
#ifndef QUATRA_UART_H
#define QUATRA_UART_H

#include "quatra_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----- ESP32 → CM5 (outbound: JSON formatters) -------------------------- */

/**
 * Format a `sensor_data` frame (PRD §8.3).
 *
 * @param buf      Destination buffer.
 * @param buf_len  Capacity of @p buf in bytes.
 * @param ts_epoch Frame timestamp (epoch seconds; integrator-supplied).
 * @param w        Latest weather inputs (caller-read).
 * @param results  Per-zone latest result snapshots.
 * @return Bytes written (including the trailing '\n'), or -1 on error
 *         (truncation, OOM, NULL inputs).
 */
int quatra_uart_format_sensor_data(char *buf, size_t buf_len,
                                   uint32_t ts_epoch,
                                   const quatra_weather_inputs_t *w,
                                   const quatra_zone_result_t results[QUATRA_NUM_ZONES]);

/**
 * Format a `valve_state` event (PRD §8.3).
 */
int quatra_uart_format_valve_state(char *buf, size_t buf_len,
                                   uint32_t ts_epoch,
                                   uint8_t  zone_id,
                                   bool     open,
                                   quatra_valve_reason_t reason,
                                   float    D_target_mm,
                                   float    ETacc_mm);

/**
 * Format an `et_daily` frame (PRD §8.3).
 *
 * @param ETc_mm   Per-zone crop ET applied today (size QUATRA_NUM_ZONES).
 * @param ETacc_mm Per-zone updated accumulator (size QUATRA_NUM_ZONES).
 */
int quatra_uart_format_et_daily(char *buf, size_t buf_len,
                                uint32_t ts_epoch,
                                float    ET0_mm,
                                uint16_t day_of_year,
                                const quatra_zone_config_t cfg[QUATRA_NUM_ZONES],
                                const float ETc_mm[QUATRA_NUM_ZONES],
                                const float ETacc_mm[QUATRA_NUM_ZONES]);

/**
 * Format an `alarm` frame (PRD §8.3).
 * Pass @p zone_id or @p sensor_idx as -1 to omit them from the JSON.
 */
int quatra_uart_format_alarm(char *buf, size_t buf_len,
                             uint32_t ts_epoch,
                             const char *code,
                             int8_t      zone_id,
                             int8_t      sensor_idx,
                             const char *detail);

/**
 * Format a `system_status` frame (PRD §8.3).
 */
int quatra_uart_format_system_status(char *buf, size_t buf_len,
                                     uint32_t ts_epoch,
                                     const char *firmware_version,
                                     bool wifi_connected,
                                     bool rtc_sync,
                                     uint32_t uptime_s,
                                     const quatra_zone_result_t results[QUATRA_NUM_ZONES]);

/* ----- CM5 → ESP32 (inbound: JSON parser) ------------------------------- */

/**
 * Parse one JSON frame into a typed command (PRD §8.4).
 *
 * @param json     NUL-terminated frame (the caller has already stripped '\n').
 * @param cmd_out  [out] Populated discriminated-union command struct.
 * @return 0 on success, -1 on malformed JSON or unknown `type`.
 */
int quatra_uart_parse_command(const char *json, quatra_uart_command_t *cmd_out);

/* ----- Convenience: stringify enums ------------------------------------- */

const char *quatra_uart_state_str (quatra_zone_state_t s);
const char *quatra_uart_reason_str(quatra_valve_reason_t r);
const char *quatra_uart_mode_str  (quatra_op_mode_t m);

#ifdef __cplusplus
}
#endif
#endif /* QUATRA_UART_H */

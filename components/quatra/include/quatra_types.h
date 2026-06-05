/**
 * @file    quatra_types.h
 * @brief   Common data types shared across the Quatra Arkansas library.
 *
 * Per PRD v1.1 amendments §2: the library is hardware-agnostic. Every
 * structure declared here is plain C, contains no FreeRTOS handles, no GPIO
 * descriptors and no driver state. The integrator owns runtime state and
 * passes data in/out as values.
 *
 * @author  Quatra Engineering
 * @date    2026-06
 * @version 1.1.0
 */
#ifndef QUATRA_TYPES_H
#define QUATRA_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of independent irrigation zones supported by the controller. */
#define QUATRA_NUM_ZONES        8
/** Watermark sensors per zone (shallow / mid / deep). */
#define QUATRA_SENSORS_PER_ZONE 3

/** Zone-level state machine — see PRD §4.2. */
typedef enum {
    QUATRA_ZONE_IDLE       = 0,  /**< Monitoring; valve closed.            */
    QUATRA_ZONE_IRRIGATING = 1,  /**< Valve open; applying water.          */
    QUATRA_ZONE_FAULT      = 2,  /**< Sensor or input out of range.        */
} quatra_zone_state_t;

/** Per-zone operating mode — see PRD §9. */
typedef enum {
    QUATRA_MODE_MANUAL    = 0,
    QUATRA_MODE_SEMI_AUTO = 1,
    QUATRA_MODE_FULL_AUTO = 2,
} quatra_op_mode_t;

/** Why a valve transition happened (carried in the JSON `valve_state`). */
typedef enum {
    QUATRA_REASON_NONE = 0,
    QUATRA_REASON_MAD_TRIGGER,
    QUATRA_REASON_SOIL_RECOVERED,
    QUATRA_REASON_ET_SATISFIED,
    QUATRA_REASON_MAX_RUNTIME,
    QUATRA_REASON_FAULT,
    QUATRA_REASON_MANUAL_CMD,
} quatra_valve_reason_t;

/**
 * Live weather inputs (read by the integrator from the RS-485 station, then
 * passed to the JSON formatter). Mirrors PRD §3.1 channel set.
 */
typedef struct {
    float temp_c;
    float humidity_pct;
    float wind_ms;
    float pressure_kpa;
    float lux;
    float rain_mm_min;
} quatra_weather_inputs_t;

/**
 * Per-zone configuration. Provided by the integrator from its own NVS
 * (Alberto's library is stateless on the agronomic side).
 */
typedef struct {
    float kc;                                   /**< Crop coefficient.        */
    float mad_cb;                               /**< MAD threshold [cb].      */
    float weights[QUATRA_SENSORS_PER_ZONE];     /**< Sensor depth weights.    */
    float area_m2;                              /**< Irrigated area [m²].     */
    float flow_m3h;                             /**< Flow rate [m³/h].        */
} quatra_zone_config_t;

/**
 * Result of one tick of the zone state machine.
 *
 * The integrator drives the valve from @p valve_open, persists @p ETacc_mm
 * to its own NVS, and forwards @p state / @p reason to the webapp via the
 * JSON formatter.
 */
typedef struct {
    bool                  valve_open;          /**< Caller must apply now.    */
    float                 psi[QUATRA_SENSORS_PER_ZONE]; /**< Pass-through inputs. */
    float                 psi_avg_cb;          /**< Weighted average.         */
    float                 D_applied_mm;        /**< Water applied so far.     */
    float                 ETacc_mm;            /**< Updated accumulator.      */
    quatra_zone_state_t   state;               /**< IDLE / IRRIGATING / FAULT.*/
    quatra_valve_reason_t reason;              /**< Why this transition.      */
} quatra_zone_result_t;

/* ----- UART command parser ----------------------------------------------- */

typedef enum {
    QUATRA_CMD_NONE          = 0,
    QUATRA_CMD_SET_PARAMS,
    QUATRA_CMD_SET_MODE,
    QUATRA_CMD_MANUAL_VALVE,
    QUATRA_CMD_REQUEST_STATUS,
    QUATRA_CMD_RESET_FAULT,
    QUATRA_CMD_RESET_ETACC,
} quatra_cmd_type_t;

#define QUATRA_SETPARAM_MASK_LATITUDE (1U << 0)
#define QUATRA_SETPARAM_MASK_HANEM    (1U << 1)

/**
 * Parsed representation of one inbound JSON command (PRD §8.4).
 * Only fields relevant to the command's type are populated.
 */
typedef struct {
    quatra_cmd_type_t type;

    /* ----- set_params --------------------------------------------------- */
    uint32_t              setp_mask;            /**< Bitmask of touched globals.  */
    float                 setp_latitude_rad;    /**< Valid if mask & LATITUDE.    */
    float                 setp_h_anemometer_m;  /**< Valid if mask & HANEM.       */
    uint8_t               setp_zone_mask;       /**< 1 bit per zone touched.      */
    quatra_zone_config_t  setp_zones[QUATRA_NUM_ZONES];

    /* ----- set_mode ----------------------------------------------------- */
    int8_t                mode_zone_id;         /**< -1 = all zones.              */
    quatra_op_mode_t      mode_value;

    /* ----- manual_valve ------------------------------------------------- */
    uint8_t               manual_zone_id;
    bool                  manual_open;

    /* ----- reset_fault / reset_ETacc ------------------------------------ */
    uint8_t               target_zone_id;
} quatra_uart_command_t;

#ifdef __cplusplus
}
#endif
#endif /* QUATRA_TYPES_H */

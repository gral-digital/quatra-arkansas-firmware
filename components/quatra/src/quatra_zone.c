/**
 * @file    quatra_zone.c
 * @brief   Per-zone irrigation control — pure function, no side effects.
 * @author  Quatra Engineering
 * @date    2026-06
 * @version 1.1.0
 *
 * Implements PRD §4.2 (states) and §5.5 (control loop). Side-effect-free:
 *   - reads no sensors,
 *   - drives no GPIO,
 *   - does no NVS / WiFi / UART I/O,
 *   - has no global state.
 *
 * Stop priority during IRRIGATING (PRD §5.5):
 *   1. Sensor fault     (≥ 2 sensors out of [0, PSI_MAX_CB])
 *   2. Soil recovered   (ψ_avg < MAD)
 *   3. ET satisfied     (D_applied ≥ ETacc, Full-Auto only)
 *   4. Safety runtime   (elapsed_s ≥ MAX_RUNTIME_S)
 */

#include "quatra_zone.h"
#include "quatra_config.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static int psi_is_invalid(float v)
{
    return !isfinite(v) || v < QUATRA_PSI_MIN_CB || v > QUATRA_PSI_MAX_CB;
}

float quatra_zone_weighted_psi(const float psi[QUATRA_SENSORS_PER_ZONE],
                               const float weights[QUATRA_SENSORS_PER_ZONE])
{
    if (psi == NULL || weights == NULL) return 0.0f;

    /* A sensor whose reading is out of [PSI_MIN, PSI_MAX] (NaN, negative,
     * adapter saturation, …) is excluded entirely from the weighted sum.
     * This prevents one bad reading from skewing the zone average when the
     * other sensors are healthy (the fault counter still tracks how many
     * sensors are unhealthy; see psi_is_invalid in the caller). */
    float w_sum = 0.0f;
    int   valid = 0;
    for (int i = 0; i < QUATRA_SENSORS_PER_ZONE; ++i) {
        if (!psi_is_invalid(psi[i]) &&
            isfinite(weights[i]) && weights[i] >= 0.0f) {
            w_sum += weights[i];
            valid++;
        }
    }

    /* PRD §5.1 fallback — all weights zero or no valid sensor → equal weights
     * over the valid sensors. If nothing is valid, return 0 (the caller is
     * expected to be in the FAULT branch in that case). */
    if (w_sum <= 1e-6f) {
        if (valid == 0) return 0.0f;
        float sum = 0.0f;
        for (int i = 0; i < QUATRA_SENSORS_PER_ZONE; ++i) {
            if (!psi_is_invalid(psi[i])) sum += psi[i];
        }
        return sum / (float)valid;
    }

    float acc = 0.0f;
    for (int i = 0; i < QUATRA_SENSORS_PER_ZONE; ++i) {
        if (!psi_is_invalid(psi[i]) &&
            isfinite(weights[i]) && weights[i] >= 0.0f) {
            acc += psi[i] * weights[i];
        }
    }
    return acc / w_sum;
}

float quatra_zone_accumulate_et(float ETacc_mm, float ETc_mm)
{
    if (!isfinite(ETacc_mm)) ETacc_mm = 0.0f;
    if (!isfinite(ETc_mm) || ETc_mm < 0.0f) ETc_mm = 0.0f;
    float out = ETacc_mm + ETc_mm;
    if (out < 0.0f) out = 0.0f;
    return out;
}

static void fill_passthrough(quatra_zone_result_t *r,
                             const float psi[QUATRA_SENSORS_PER_ZONE],
                             float psi_avg)
{
    for (int i = 0; i < QUATRA_SENSORS_PER_ZONE; ++i) {
        r->psi[i] = isfinite(psi[i]) ? psi[i] : 0.0f;
    }
    r->psi_avg_cb = psi_avg;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

quatra_zone_result_t quatra_zone_update(uint8_t          zone_id,
                                        const float      psi[QUATRA_SENSORS_PER_ZONE],
                                        const float      weights[QUATRA_SENSORS_PER_ZONE],
                                        float            MAD_cb,
                                        float            ETacc_mm,
                                        float            Q_m3h,
                                        float            area_m2,
                                        uint32_t         elapsed_s,
                                        quatra_op_mode_t mode)
{
    (void)zone_id; /* Diagnostics only; not used in the math. */

    quatra_zone_result_t r;
    memset(&r, 0, sizeof(r));
    r.ETacc_mm = isfinite(ETacc_mm) ? ETacc_mm : 0.0f;
    r.reason   = QUATRA_REASON_NONE;

    /* Defensive copies of the input arrays so we can echo them back. */
    float psi_local[QUATRA_SENSORS_PER_ZONE];
    int   fault_count = 0;
    for (int i = 0; i < QUATRA_SENSORS_PER_ZONE; ++i) {
        psi_local[i] = (psi != NULL) ? psi[i] : NAN;
        if (psi_is_invalid(psi_local[i])) fault_count++;
    }
    float psi_avg = quatra_zone_weighted_psi(psi_local, weights);

    /* ---------------------- FAULT branch ------------------------ */
    if (fault_count >= 2) {
        fill_passthrough(&r, psi_local, psi_avg);
        r.valve_open = false;
        r.state      = QUATRA_ZONE_FAULT;
        r.reason     = QUATRA_REASON_FAULT;
        return r;
    }

    /* ---------------------- MANUAL branch ----------------------- */
    if (mode == QUATRA_MODE_MANUAL) {
        fill_passthrough(&r, psi_local, psi_avg);
        r.valve_open = false;          /* Caller drives the valve directly.  */
        r.state      = QUATRA_ZONE_IDLE;
        r.reason     = QUATRA_REASON_MANUAL_CMD;
        return r;
    }

    fill_passthrough(&r, psi_local, psi_avg);

    /* ---------------------- IDLE → IRRIGATING ------------------- */
    if (elapsed_s == 0) {
        bool soil_calls_water = (psi_avg >= MAD_cb);
        bool needs_et         = (mode == QUATRA_MODE_FULL_AUTO);
        bool start_ok         = soil_calls_water;

        if (start_ok && needs_et && r.ETacc_mm <= 0.0f) {
            start_ok = false;     /* Full-Auto requires an outstanding ET debt. */
        }

        if (start_ok) {
            r.valve_open   = true;
            r.state        = QUATRA_ZONE_IRRIGATING;
            r.reason       = QUATRA_REASON_MAD_TRIGGER;
            r.D_applied_mm = 0.0f;
        } else {
            r.valve_open   = false;
            r.state        = QUATRA_ZONE_IDLE;
            r.reason       = QUATRA_REASON_NONE;
        }
        return r;
    }

    /* ---------------------- IRRIGATING tick --------------------- */
    /* Water applied so far (PRD §5.4): V = Q/3600 × t, D = V/area × 1000. */
    if (isfinite(Q_m3h) && Q_m3h > 0.0f &&
        isfinite(area_m2) && area_m2 > 0.0f) {
        float V_applied = Q_m3h / 3600.0f * (float)elapsed_s;
        r.D_applied_mm  = V_applied / area_m2 * 1000.0f;
    } else {
        r.D_applied_mm  = 0.0f;
    }

    /* Stop conditions in priority order (PRD §5.5). */
    if (psi_avg < MAD_cb) {
        r.valve_open = false;
        r.state      = QUATRA_ZONE_IDLE;
        r.reason     = QUATRA_REASON_SOIL_RECOVERED;
        r.ETacc_mm   = 0.0f;
        return r;
    }
    if (mode == QUATRA_MODE_FULL_AUTO &&
        r.ETacc_mm > 0.0f && r.D_applied_mm >= r.ETacc_mm) {
        r.valve_open = false;
        r.state      = QUATRA_ZONE_IDLE;
        r.reason     = QUATRA_REASON_ET_SATISFIED;
        r.ETacc_mm   = 0.0f;
        return r;
    }
    if (elapsed_s >= QUATRA_MAX_RUNTIME_S) {
        r.valve_open = false;
        r.state      = QUATRA_ZONE_IDLE;
        r.reason     = QUATRA_REASON_MAX_RUNTIME;
        r.ETacc_mm   = 0.0f;
        return r;
    }

    /* Still irrigating. */
    r.valve_open = true;
    r.state      = QUATRA_ZONE_IRRIGATING;
    r.reason     = QUATRA_REASON_NONE;
    return r;
}

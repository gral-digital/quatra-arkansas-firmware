/**
 * @file    quatra_zone.h
 * @brief   Per-zone irrigation control — pure function, no side effects.
 *
 * Per PRD v1.1 amendments §4 the zone module is a pure function: given the
 * current inputs (soil tension, mode, accumulated ET, elapsed time, …) it
 * returns the action the integrator must take next (valve_open, next state,
 * updated accumulator).
 *
 * The integrator owns:
 *   - the wall clock (it computes @p elapsed_s),
 *   - the persistence of @p ETacc_mm across reboots,
 *   - the physical valve drive (acts on @p valve_open in the result),
 *   - the FreeRTOS scheduling around this call.
 */
#ifndef QUATRA_ZONE_H
#define QUATRA_ZONE_H

#include "quatra_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Weighted root-zone tension average.
 *        Implements PRD §5.1 with the (w1+w2+w3)==0 fallback to equal weights.
 */
float quatra_zone_weighted_psi(const float psi[QUATRA_SENSORS_PER_ZONE],
                               const float weights[QUATRA_SENSORS_PER_ZONE]);

/**
 * @brief One tick of the per-zone state machine.
 *
 * Caller convention for the current state:
 *   - @p elapsed_s == 0  → zone is currently IDLE (or just transitioned in).
 *   - @p elapsed_s  > 0  → zone is currently IRRIGATING; @p elapsed_s is
 *                          the number of seconds since the irrigation started.
 *
 * The function inspects soil tension, mode and ETacc, then returns:
 *   - @c valve_open      → what the integrator should set the relay to NOW.
 *   - @c state           → the new zone state.
 *   - @c reason          → human-readable rationale for the transition.
 *   - @c D_applied_mm    → water depth applied so far in the current session.
 *   - @c ETacc_mm        → updated accumulator. Reset to 0 when irrigation
 *                          terminates normally (PRD §5.5).
 *   - @c psi[3] / @c psi_avg_cb → echoed/derived for the JSON formatter.
 *
 * @note In MANUAL mode the function returns a safe baseline
 *       (valve_open=false, state=IDLE) — the integrator drives the valve
 *       directly in that mode and should not rely on this output.
 *
 * @param zone_id   Zone index in [0, QUATRA_NUM_ZONES). Used for diagnostics only.
 * @param psi       Three Watermark readings [cb] already converted to centibar
 *                  by the upstream adapter (see PRD v1.1 amendments §6).
 * @param weights   Per-depth weights (typically {1, 2, 3}).
 * @param MAD_cb    Management Allowable Depletion threshold [cb].
 * @param ETacc_mm  Current accumulated ET [mm]. Caller keeps this constant
 *                  during an active irrigation session.
 * @param Q_m3h     Flow rate for this zone [m³/h].
 * @param area_m2   Irrigated area [m²].
 * @param elapsed_s 0 if IDLE, else seconds since irrigation start.
 * @param mode      MANUAL / SEMI_AUTO / FULL_AUTO.
 */
quatra_zone_result_t quatra_zone_update(uint8_t          zone_id,
                                        const float      psi[QUATRA_SENSORS_PER_ZONE],
                                        const float      weights[QUATRA_SENSORS_PER_ZONE],
                                        float            MAD_cb,
                                        float            ETacc_mm,
                                        float            Q_m3h,
                                        float            area_m2,
                                        uint32_t         elapsed_s,
                                        quatra_op_mode_t mode);

/**
 * @brief Pure helper — apply one daily ETc contribution to an accumulator.
 *
 * @return ETacc_mm + max(0, ETc_mm). Guards against NaN/Inf.
 */
float quatra_zone_accumulate_et(float ETacc_mm, float ETc_mm);

#ifdef __cplusplus
}
#endif
#endif /* QUATRA_ZONE_H */

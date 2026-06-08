/**
 * @file    quatra_config.h
 * @brief   Algorithm constants and tunable defaults.
 *
 * Per PRD v1.1 amendments §2 the library has no hardware concerns, so this
 * header contains ONLY the numeric bounds and defaults that drive the math
 * in quatra_et0.c and quatra_zone.c. No GPIO map, no UART pins, no RTOS
 * priorities — those belong to the integrator's main application.
 */
#ifndef QUATRA_CONFIG_H
#define QUATRA_CONFIG_H

#include "quatra_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
 * Versioning
 * -------------------------------------------------------------------------- */
#define QUATRA_FIRMWARE_VERSION        "1.1.2"

/* --------------------------------------------------------------------------
 * Algorithm defaults  (PRD §12)
 * -------------------------------------------------------------------------- */
/** Default site latitude [radians]. 45° N (northern Italy). */
#define QUATRA_LATITUDE_RAD            0.7854f
/** Default anemometer mounting height [m]. FAO-56 reference. */
#define QUATRA_H_ANEMOMETER            2.0f
/** Per-zone hard safety cap on irrigation duration [s]. */
#define QUATRA_MAX_RUNTIME_S           3600U
/** Lux → W/m² approximation used by integrator-side solar accumulation
 *  (PRD §5.2 step 6, §16.1). Exposed here so client code can share it. */
#define QUATRA_LUX_TO_WM2              120.0f
/** Maximum number of zones that may irrigate simultaneously. */
#define QUATRA_PARALLEL_ZONES          1

/* --------------------------------------------------------------------------
 * Numerical guards  (PRD §11.3)
 * -------------------------------------------------------------------------- */
/** Maximum plausible ET0 [mm/day]; values above are clamped. */
#define QUATRA_ET0_MAX_MM_DAY          20.0f
/** Watermark 200SS soil-tension range, per amendment §5. */
#define QUATRA_PSI_MAX_CB              239.0f
#define QUATRA_PSI_MIN_CB                0.0f

/* --------------------------------------------------------------------------
 * Per-zone agronomic defaults  (PRD §12.2)
 * -------------------------------------------------------------------------- */
#define QUATRA_DEFAULT_KC              1.0f
#define QUATRA_DEFAULT_MAD_CB          50.0f
#define QUATRA_DEFAULT_WEIGHTS         { 1.0f, 2.0f, 3.0f }
#define QUATRA_DEFAULT_AREA_M2         1000.0f
#define QUATRA_DEFAULT_FLOW_M3H        10.0f
#define QUATRA_DEFAULT_MODE            QUATRA_MODE_FULL_AUTO

#ifdef __cplusplus
}
#endif
#endif /* QUATRA_CONFIG_H */

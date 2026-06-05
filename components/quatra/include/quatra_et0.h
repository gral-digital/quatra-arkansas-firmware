/**
 * @file    quatra_et0.h
 * @brief   FAO-56 Penman-Monteith reference evapotranspiration (ET0).
 *
 * Pure-math module — no ESP-IDF, no FreeRTOS, no hardware dependency.
 * Compiles and runs on any C99 host.
 */
#ifndef QUATRA_ET0_H
#define QUATRA_ET0_H

#include "quatra_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute daily reference evapotranspiration ET0 [mm/day].
 *
 * Implements steps 2 → 13 of the FAO-56 Penman-Monteith equation as
 * specified in PRD §5.2. The wind-speed-at-2m correction (step 1) is split
 * out as quatra_et0_wind_at_2m() so the caller can apply it once when it
 * reads the weather station.
 *
 * @param T_c           Mean daily air temperature [°C].
 * @param RH_pct        Mean daily relative humidity [%].
 * @param u2_ms         Wind speed at 2 m height [m/s]. Use
 *                      quatra_et0_wind_at_2m() if the station is not at 2 m.
 * @param P_kpa         Mean daily atmospheric pressure [kPa].
 * @param Rs_MJm2       Integrated daily solar radiation [MJ/m²/day].
 * @param latitude_rad  Site latitude [radians] (positive North).
 * @param day_of_year   Day-of-year in [1, 366].
 *
 * @return ET0 in mm/day, clamped to [0, QUATRA_ET0_MAX_MM_DAY].
 */
float quatra_et0_compute(float T_c,
                         float RH_pct,
                         float u2_ms,
                         float P_kpa,
                         float Rs_MJm2,
                         float latitude_rad,
                         int   day_of_year);

/**
 * @brief Project wind speed measured at height @p h_m to the FAO-56
 *        reference height of 2 m. PRD §5.2 step 1.
 *
 * @param u_h_ms    Wind speed at the anemometer height [m/s].
 * @param h_m       Anemometer mounting height [m].
 * @return Wind speed at 2 m [m/s]. If @p h_m == 2.0 the input is returned
 *         unchanged. Guarded against invalid heights (returns @p u_h_ms).
 */
float quatra_et0_wind_at_2m(float u_h_ms, float h_m);

#ifdef __cplusplus
}
#endif
#endif /* QUATRA_ET0_H */

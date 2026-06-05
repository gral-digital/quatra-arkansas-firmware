/**
 * @file    quatra_et0.c
 * @brief   FAO-56 Penman-Monteith reference evapotranspiration (ET0).
 * @author  Quatra Engineering
 * @date    2026-06
 * @version 1.1.0
 *
 * Pure-math implementation of PRD §5.2 (Steps 1 → 13). No ESP-IDF, no
 * FreeRTOS, no hardware dependency — compiles cleanly with `gcc -std=c99`
 * and is unit-tested on the host (see tests/test_et0.c).
 *
 * All inputs are sanitised against NaN/Inf and clamped to plausible ranges
 * before they enter the equation (PRD §11.3).
 */

#include "quatra_et0.h"
#include "quatra_config.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

static inline float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* ------------------------------------------------------------------------- */
/* Step 1 (split out so the caller can apply it once at sensor read time)    */
/* ------------------------------------------------------------------------- */

float quatra_et0_wind_at_2m(float u_h_ms, float h_m)
{
    if (!isfinite(u_h_ms) || u_h_ms < 0.0f) return 0.0f;
    if (!isfinite(h_m) || h_m <= 0.0f)      return u_h_ms;
    if (fabsf(h_m - 2.0f) < 1e-3f)          return u_h_ms;

    float denom = logf(67.8f * h_m - 5.42f);
    if (denom <= 0.0f) return u_h_ms;
    return u_h_ms * 4.87f / denom;
}

/* ------------------------------------------------------------------------- */
/* Public API — full ET0 computation                                         */
/* ------------------------------------------------------------------------- */

float quatra_et0_compute(float T,
                         float RH,
                         float u2,
                         float P,
                         float Rs_daily,
                         float latitude_rad,
                         int   day_of_year)
{
    if (!isfinite(T) || !isfinite(RH) || !isfinite(u2) ||
        !isfinite(P) || !isfinite(Rs_daily) || !isfinite(latitude_rad)) {
        return 0.0f;
    }
    T          = clampf(T,          -40.0f,  80.0f);
    RH         = clampf(RH,           0.0f, 100.0f);
    u2         = clampf(u2,           0.0f,  60.0f);
    P          = clampf(P,           50.0f, 120.0f);
    Rs_daily   = clampf(Rs_daily,     0.0f,  60.0f);
    if (day_of_year < 1)   day_of_year = 1;
    if (day_of_year > 366) day_of_year = 366;

    /* --------------------------------------------------------------------- *
     * Step 2 — Saturation vapor pressure e_s [kPa]                          *
     * --------------------------------------------------------------------- */
    float e_s = 0.6108f * expf(17.27f * T / (T + 237.3f));

    /* --------------------------------------------------------------------- *
     * Step 3 — Actual vapor pressure e_a [kPa]                              *
     * --------------------------------------------------------------------- */
    float e_a = e_s * RH / 100.0f;

    /* --------------------------------------------------------------------- *
     * Step 4 — Slope of saturation vapor pressure curve Δ [kPa/°C]          *
     * --------------------------------------------------------------------- */
    float t237  = T + 237.3f;
    float delta = 4098.0f * e_s / (t237 * t237);

    /* --------------------------------------------------------------------- *
     * Step 5 — Psychrometric constant γ [kPa/°C] (from measured pressure)   *
     * --------------------------------------------------------------------- */
    float gamma = 0.000665f * P;

    /* --------------------------------------------------------------------- *
     * Step 6 — Rs_daily provided pre-accumulated by the caller.             *
     * --------------------------------------------------------------------- */

    /* --------------------------------------------------------------------- *
     * Step 7 — Extraterrestrial radiation Ra [MJ/m²/day]                    *
     * --------------------------------------------------------------------- */
    float angle = 2.0f * M_PI * (float)day_of_year / 365.0f;
    float dr    = 1.0f + 0.033f * cosf(angle);
    float decl  = 0.409f * sinf(angle - 1.39f);

    float omega_arg = -tanf(latitude_rad) * tanf(decl);
    omega_arg       = clampf(omega_arg, -1.0f, 1.0f);
    float omega_s   = acosf(omega_arg);

    float Ra = (24.0f * 60.0f / M_PI) * 0.0820f * dr *
               (omega_s * sinf(latitude_rad) * sinf(decl) +
                cosf(latitude_rad) * cosf(decl) * sinf(omega_s));
    if (Ra < 0.0f) Ra = 0.0f;

    /* --------------------------------------------------------------------- *
     * Step 8 — Altitude z [m] from pressure (barometric formula)            *
     * --------------------------------------------------------------------- */
    float z_m = (293.0f / 0.0065f) *
                (1.0f - powf(P / 101.325f, 1.0f / 5.256f));

    /* --------------------------------------------------------------------- *
     * Step 9 — Clear-sky solar radiation Rs0 [MJ/m²/day]                    *
     * --------------------------------------------------------------------- */
    float Rs0 = (0.75f + 2e-5f * z_m) * Ra;
    if (Rs0 <= 0.0f) Rs0 = 0.01f;

    /* --------------------------------------------------------------------- *
     * Step 10 — Net shortwave radiation Rns [MJ/m²/day]                     *
     * --------------------------------------------------------------------- */
    float Rns = 0.77f * Rs_daily;

    /* --------------------------------------------------------------------- *
     * Step 11 — Net longwave radiation Rnl [MJ/m²/day]                      *
     * --------------------------------------------------------------------- */
    float cloud_factor = 1.35f * Rs_daily / Rs0 - 0.35f;
    cloud_factor = clampf(cloud_factor, 0.05f, 1.0f);

    float Tk  = T + 273.16f;
    float Tk4 = Tk * Tk * Tk * Tk;
    float Rnl = 4.903e-9f * Tk4 *
                (0.34f - 0.14f * sqrtf(e_a)) * cloud_factor;
    if (Rnl < 0.0f) Rnl = 0.0f;

    /* --------------------------------------------------------------------- *
     * Step 12 — Net radiation Rn = Rns − Rnl [MJ/m²/day]                    *
     * --------------------------------------------------------------------- */
    float Rn = Rns - Rnl;
    float G  = 0.0f;                  /* daily timestep: G ≈ 0              */

    /* --------------------------------------------------------------------- *
     * Step 13 — ET0 [mm/day]                                                *
     * --------------------------------------------------------------------- */
    float numerator   = 0.408f * delta * (Rn - G) +
                        gamma * (900.0f / (T + 273.0f)) * u2 * (e_s - e_a);
    float denominator = delta + gamma * (1.0f + 0.34f * u2);

    float ET0 = (denominator > 0.0f) ? (numerator / denominator) : 0.0f;
    return clampf(ET0, 0.0f, QUATRA_ET0_MAX_MM_DAY);
}

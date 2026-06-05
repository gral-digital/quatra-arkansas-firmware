/**
 * @file test_et0.c
 * @brief Unit tests for the FAO-56 ET0 implementation.
 *
 * Reference numbers are taken from the worked example in FAO Irrigation and
 * Drainage Paper No. 56 (Example 17, Bangkok, Thailand) — the canonical
 * validation case for any FAO-56 implementation. Reference ET0 ≈ 3.9 mm/day.
 *
 * Build:  make test-et0
 * Run:    ./build/test_et0
 */

#include "test_common.h"
#include "quatra_et0.h"
#include "quatra_config.h"

#include <math.h>

static int test_wind_correction(void)
{
    TEST_BEGIN("wind_at_2m");

    /* h == 2 → no change. */
    CHECK_NEAR(quatra_et0_wind_at_2m(3.0f, 2.0f), 3.0f, 1e-6);

    /* Example FAO-56 Box 7: u_h = 3.2 m/s at 10 m → u2 ≈ 2.4 m/s. */
    CHECK_NEAR(quatra_et0_wind_at_2m(3.2f, 10.0f), 2.4f, 0.05);

    /* Guard: invalid height returns input. */
    CHECK_NEAR(quatra_et0_wind_at_2m(2.0f, -1.0f), 2.0f, 1e-6);
    CHECK_NEAR(quatra_et0_wind_at_2m(2.0f, 0.0f),  2.0f, 1e-6);

    /* Guard: NaN / negative wind. */
    CHECK_NEAR(quatra_et0_wind_at_2m(NAN, 2.0f), 0.0f, 1e-6);
    CHECK_NEAR(quatra_et0_wind_at_2m(-1.0f, 2.0f), 0.0f, 1e-6);

    TEST_END();
    return 0;
}

static int test_clamps(void)
{
    TEST_BEGIN("clamps");

    /* All NaN → 0 (degenerate but must not crash). */
    float v = quatra_et0_compute(NAN, NAN, NAN, NAN, NAN, 0.0f, 100);
    CHECK_NEAR(v, 0.0f, 1e-6);

    /* Hot dry windy day — extreme but within bounds, ET0 should be positive
     * and below the safety ceiling. */
    v = quatra_et0_compute(45.0f, 5.0f, 8.0f, 95.0f, 30.0f, 0.5235f /* 30°N */, 200);
    CHECK_TRUE(v > 5.0f);
    CHECK_TRUE(v <= QUATRA_ET0_MAX_MM_DAY);

    /* Cold cloudy still day with no radiation → ET0 should be very small. */
    v = quatra_et0_compute(2.0f, 95.0f, 0.5f, 100.0f, 1.0f, 0.7854f, 350);
    CHECK_TRUE(v >= 0.0f);
    CHECK_TRUE(v < 1.0f);

    /* Out-of-range pressure is silently clamped (no crash). */
    v = quatra_et0_compute(20.0f, 60.0f, 2.0f, 1.0f, 15.0f, 0.5f, 150);
    CHECK_TRUE(isfinite(v));

    TEST_END();
    return 0;
}

/* FAO-56 Example 17 (Bangkok, Thailand, April).
 * Inputs:
 *   T_min = 25.6 °C, T_max = 34.8 °C  → T_mean = 30.2 °C
 *   RH_mean ≈ 73 %        (RH_min 50%, RH_max 96% → use mean for PoC)
 *   u2 = 2.0 m/s
 *   P  = 101.3 kPa  (sea-level approximation)
 *   Rs = 22.65 MJ/m²/day  (clear-sky solar radiation)
 *   φ  = 0.2393 rad (13.73°N)
 *   J  = 105
 * Expected ET0 ≈ 5.7 mm/day from the FAO worked example.
 * Our daily-mean implementation (single T, daily-mean RH) gives a result
 * within ±0.8 mm/day of the reference — acceptable for the PoC scope
 * (PRD §16.6 notes the Tmax/Tmin improvement as future work).
 */
static int test_fao56_example_17(void)
{
    TEST_BEGIN("fao56_example_17");

    float et0 = quatra_et0_compute(
        /* T_c           */ 30.2f,
        /* RH_pct        */ 73.0f,
        /* u2_ms         */ 2.0f,
        /* P_kpa         */ 101.3f,
        /* Rs_MJm2       */ 22.65f,
        /* latitude_rad  */ 0.2393f,
        /* day_of_year   */ 105
    );

    printf("  ET0(Bangkok, Apr) = %.3f mm/day (reference ≈ 5.7)\n", et0);
    CHECK_NEAR(et0, 5.7, 1.0);
    CHECK_TRUE(et0 > 0.0f);
    CHECK_TRUE(et0 <= QUATRA_ET0_MAX_MM_DAY);

    TEST_END();
    return 0;
}

/* Plausibility regression: northern-Italy summer day.
 * 28°C, 50% RH, 1.5 m/s wind, 25 MJ/m²/day Rs, 45°N, July → 5-7 mm/day. */
static int test_north_italy_summer(void)
{
    TEST_BEGIN("north_italy_summer");

    float et0 = quatra_et0_compute(
        28.0f, 50.0f, 1.5f, 100.5f, 25.0f, 0.7854f /* 45°N */, 200);
    printf("  ET0(N-Italy, Jul) = %.3f mm/day (expected 5-7)\n", et0);
    CHECK_TRUE(et0 >= 4.0f);
    CHECK_TRUE(et0 <= 8.0f);

    TEST_END();
    return 0;
}

/* Winter / overcast day — ET0 should be small but non-negative. */
static int test_winter_overcast(void)
{
    TEST_BEGIN("winter_overcast");

    float et0 = quatra_et0_compute(
        5.0f, 90.0f, 1.0f, 101.0f, 3.0f, 0.7854f, 15);
    printf("  ET0(N-Italy, Jan, overcast) = %.3f mm/day\n", et0);
    CHECK_TRUE(et0 >= 0.0f);
    CHECK_TRUE(et0 < 2.0f);

    TEST_END();
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= test_wind_correction();
    rc |= test_clamps();
    rc |= test_fao56_example_17();
    rc |= test_north_italy_summer();
    rc |= test_winter_overcast();

    if (rc == 0) printf("\nall ET0 tests passed.\n");
    return rc;
}

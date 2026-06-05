/**
 * @file test_zone.c
 * @brief Unit tests for quatra_zone_update — pure function exercise.
 *
 * Build:  make test-zone
 * Run:    ./build/test_zone
 */

#include "test_common.h"
#include "quatra_zone.h"
#include "quatra_config.h"

static const float W_DEFAULT[QUATRA_SENSORS_PER_ZONE] = { 1.0f, 2.0f, 3.0f };

static int test_weighted_psi(void)
{
    TEST_BEGIN("weighted_psi");

    float psi[3]     = { 30.0f, 50.0f, 70.0f };
    float weights[3] = { 1.0f, 2.0f, 3.0f };
    /* (30 + 100 + 210) / 6 = 56.666... */
    CHECK_NEAR(quatra_zone_weighted_psi(psi, weights), 56.6667, 1e-3);

    /* All weights zero → fallback equal weights average = 50. */
    float wz[3] = { 0.0f, 0.0f, 0.0f };
    CHECK_NEAR(quatra_zone_weighted_psi(psi, wz), 50.0, 1e-6);

    /* Regression: a single out-of-range sensor must be excluded from the
     * weighted sum, NOT skew the average upward. */
    float psi_bad[3] = { 999.0f, 30.0f, 40.0f };
    /* Valid sensors are index 1 (w=2) and index 2 (w=3).
     * weighted avg = (30*2 + 40*3) / (2+3) = 180/5 = 36.0 */
    CHECK_NEAR(quatra_zone_weighted_psi(psi_bad, weights), 36.0, 1e-3);

    /* NaN sensor is treated identically (excluded). */
    float psi_nan[3] = { NAN, 30.0f, 40.0f };
    CHECK_NEAR(quatra_zone_weighted_psi(psi_nan, weights), 36.0, 1e-3);

    /* Negative sensor is treated identically (excluded). */
    float psi_neg[3] = { -5.0f, 30.0f, 40.0f };
    CHECK_NEAR(quatra_zone_weighted_psi(psi_neg, weights), 36.0, 1e-3);

    /* All sensors invalid → returns 0 (caller is expected to be in FAULT). */
    float psi_all_bad[3] = { 999.0f, NAN, -1.0f };
    CHECK_NEAR(quatra_zone_weighted_psi(psi_all_bad, weights), 0.0, 1e-6);

    /* NULL safety. */
    CHECK_NEAR(quatra_zone_weighted_psi(NULL, weights), 0.0, 1e-6);
    CHECK_NEAR(quatra_zone_weighted_psi(psi, NULL),    0.0, 1e-6);

    TEST_END();
    return 0;
}

/* Regression: one out-of-range sensor must NOT trigger spurious irrigation. */
static int test_single_invalid_sensor_does_not_skew_decision(void)
{
    TEST_BEGIN("single_invalid_sensor_no_spurious_start");

    /* Two sensors say "soil is wet" (ψ=30,40 << MAD=50).
     * One sensor saturates to 999 (adapter fault).
     * Old buggy behavior: weighted avg ≈ 196 → START irrigation.
     * Correct behavior: invalid excluded, weighted avg ≈ 36 → DO NOT start. */
    float psi[3]     = { 999.0f, 30.0f, 40.0f };
    float weights[3] = { 1.0f, 2.0f, 3.0f };

    quatra_zone_result_t r = quatra_zone_update(
        0, psi, weights, 50.0f /* MAD */, 12.0f /* ETacc */,
        10.0f, 1000.0f, 0, QUATRA_MODE_FULL_AUTO);

    CHECK_FALSE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_IDLE);
    CHECK_TRUE(r.psi_avg_cb < 50.0f);
    TEST_END();
    return 0;
}

/* Defensive: zero flow / zero area must not crash or divide by zero. */
static int test_zero_flow_zero_area_safe(void)
{
    TEST_BEGIN("zero_flow_or_area_safe");

    float psi[3] = { 60.0f, 65.0f, 70.0f };

    /* Q = 0 during IRRIGATING → D_applied stays 0, stop only on soil/runtime. */
    quatra_zone_result_t r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 12.0f,
        0.0f /* Q */, 1000.0f /* area */, 500, QUATRA_MODE_FULL_AUTO);
    CHECK_NEAR(r.D_applied_mm, 0.0, 1e-6);
    CHECK_TRUE(r.valve_open);   /* still irrigating; ET cap never reached */

    /* area = 0 same effect. */
    r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 12.0f, 10.0f, 0.0f, 500,
        QUATRA_MODE_FULL_AUTO);
    CHECK_NEAR(r.D_applied_mm, 0.0, 1e-6);
    CHECK_TRUE(r.valve_open);

    /* NaN flow same effect. */
    r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 12.0f, NAN, 1000.0f, 500,
        QUATRA_MODE_FULL_AUTO);
    CHECK_NEAR(r.D_applied_mm, 0.0, 1e-6);

    TEST_END();
    return 0;
}

/* Defensive: NULL psi / NULL weights must not crash. */
static int test_null_inputs_safe(void)
{
    TEST_BEGIN("null_inputs_safe");

    quatra_zone_result_t r = quatra_zone_update(
        0, NULL, W_DEFAULT, 50.0f, 12.0f, 10.0f, 1000.0f, 0,
        QUATRA_MODE_FULL_AUTO);
    /* All-NaN psi → 3 invalid sensors → FAULT branch. */
    CHECK_EQ(r.state, QUATRA_ZONE_FAULT);
    CHECK_FALSE(r.valve_open);

    float psi[3] = { 30.0f, 40.0f, 50.0f };
    r = quatra_zone_update(0, psi, NULL, 50.0f, 12.0f, 10.0f, 1000.0f, 0,
                           QUATRA_MODE_FULL_AUTO);
    /* NULL weights → weighted_psi returns 0 → below MAD → IDLE. No crash. */
    CHECK_EQ(r.state, QUATRA_ZONE_IDLE);
    CHECK_FALSE(r.valve_open);
    TEST_END();
    return 0;
}

static int test_idle_does_not_start_when_dry_threshold_not_met(void)
{
    TEST_BEGIN("idle_below_mad");

    float psi[3] = { 30.0f, 35.0f, 40.0f }; /* avg ≈ 36.66 */
    quatra_zone_result_t r = quatra_zone_update(
        /* zone_id */ 0, psi, W_DEFAULT,
        /* MAD     */ 50.0f,
        /* ETacc   */ 10.0f,
        /* Q       */ 10.0f, /* area */ 1000.0f,
        /* elapsed */ 0,    QUATRA_MODE_FULL_AUTO);

    CHECK_FALSE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_IDLE);
    CHECK_NEAR(r.ETacc_mm, 10.0, 1e-6); /* unchanged */
    TEST_END();
    return 0;
}

static int test_idle_starts_when_mad_reached_full_auto(void)
{
    TEST_BEGIN("idle_starts_full_auto");

    float psi[3] = { 60.0f, 65.0f, 70.0f }; /* avg ≈ 66.66 */
    quatra_zone_result_t r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 12.0f, 10.0f, 1000.0f, 0,
        QUATRA_MODE_FULL_AUTO);

    CHECK_TRUE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_IRRIGATING);
    CHECK_EQ(r.reason, QUATRA_REASON_MAD_TRIGGER);
    CHECK_NEAR(r.D_applied_mm, 0.0, 1e-6);
    TEST_END();
    return 0;
}

static int test_full_auto_requires_eta_debt(void)
{
    TEST_BEGIN("full_auto_no_etacc_no_start");

    float psi[3] = { 60.0f, 65.0f, 70.0f };
    quatra_zone_result_t r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 0.0f /* no debt */, 10.0f, 1000.0f, 0,
        QUATRA_MODE_FULL_AUTO);

    CHECK_FALSE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_IDLE);
    TEST_END();
    return 0;
}

static int test_semi_auto_starts_without_etacc(void)
{
    TEST_BEGIN("semi_auto_starts_without_etacc");

    float psi[3] = { 60.0f, 65.0f, 70.0f };
    quatra_zone_result_t r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 0.0f, 10.0f, 1000.0f, 0,
        QUATRA_MODE_SEMI_AUTO);

    CHECK_TRUE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_IRRIGATING);
    TEST_END();
    return 0;
}

static int test_stop_on_soil_recovered(void)
{
    TEST_BEGIN("stop_soil_recovered");

    /* Currently irrigating, soil now < MAD → must stop AND reset ETacc. */
    float psi[3] = { 30.0f, 35.0f, 40.0f }; /* avg ~36, MAD 50 */
    quatra_zone_result_t r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 12.0f, 10.0f, 1000.0f,
        /* elapsed */ 120, QUATRA_MODE_FULL_AUTO);

    CHECK_FALSE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_IDLE);
    CHECK_EQ(r.reason, QUATRA_REASON_SOIL_RECOVERED);
    CHECK_NEAR(r.ETacc_mm, 0.0, 1e-6);
    TEST_END();
    return 0;
}

static int test_stop_on_et_satisfied(void)
{
    TEST_BEGIN("stop_et_satisfied");

    /* Q=10 m³/h, area=1000 m². D_applied = Q/3600 × t / area × 1000.
     * To apply 5 mm we need t = 5 × 1000 × 3600 / (10 × 1000) = 1800 s. */
    float psi[3] = { 60.0f, 65.0f, 70.0f }; /* still > MAD */
    quatra_zone_result_t r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 5.0f /* ETacc target */, 10.0f, 1000.0f,
        1800, QUATRA_MODE_FULL_AUTO);

    CHECK_FALSE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_IDLE);
    CHECK_EQ(r.reason, QUATRA_REASON_ET_SATISFIED);
    CHECK_NEAR(r.D_applied_mm, 5.0, 1e-3);
    CHECK_NEAR(r.ETacc_mm, 0.0, 1e-6);
    TEST_END();
    return 0;
}

static int test_stop_on_max_runtime(void)
{
    TEST_BEGIN("stop_max_runtime");

    float psi[3] = { 60.0f, 65.0f, 70.0f };
    quatra_zone_result_t r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 100.0f /* huge debt */, 1.0f /* tiny flow */,
        1000.0f, QUATRA_MAX_RUNTIME_S, QUATRA_MODE_FULL_AUTO);

    CHECK_FALSE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_IDLE);
    CHECK_EQ(r.reason, QUATRA_REASON_MAX_RUNTIME);
    TEST_END();
    return 0;
}

static int test_fault_on_two_invalid_sensors(void)
{
    TEST_BEGIN("fault_two_invalid_sensors");

    float psi[3] = { -10.0f, 999.0f, 50.0f };
    quatra_zone_result_t r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 10.0f, 10.0f, 1000.0f, 0,
        QUATRA_MODE_FULL_AUTO);

    CHECK_FALSE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_FAULT);
    CHECK_EQ(r.reason, QUATRA_REASON_FAULT);
    TEST_END();
    return 0;
}

static int test_manual_mode_returns_safe_baseline(void)
{
    TEST_BEGIN("manual_safe_baseline");

    float psi[3] = { 60.0f, 65.0f, 70.0f };
    quatra_zone_result_t r = quatra_zone_update(
        0, psi, W_DEFAULT, 50.0f, 12.0f, 10.0f, 1000.0f, 0,
        QUATRA_MODE_MANUAL);

    CHECK_FALSE(r.valve_open);
    CHECK_EQ(r.state, QUATRA_ZONE_IDLE);
    CHECK_EQ(r.reason, QUATRA_REASON_MANUAL_CMD);
    TEST_END();
    return 0;
}

static int test_accumulate_et(void)
{
    TEST_BEGIN("accumulate_et");

    CHECK_NEAR(quatra_zone_accumulate_et(2.0f,  3.5f), 5.5,  1e-6);
    CHECK_NEAR(quatra_zone_accumulate_et(0.0f,  0.0f), 0.0,  1e-6);
    CHECK_NEAR(quatra_zone_accumulate_et(NAN,   3.0f), 3.0,  1e-6);
    CHECK_NEAR(quatra_zone_accumulate_et(2.0f, -5.0f), 2.0,  1e-6); /* negative dropped */
    CHECK_NEAR(quatra_zone_accumulate_et(2.0f,  NAN ), 2.0,  1e-6);

    TEST_END();
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= test_weighted_psi();
    rc |= test_single_invalid_sensor_does_not_skew_decision();
    rc |= test_zero_flow_zero_area_safe();
    rc |= test_null_inputs_safe();
    rc |= test_idle_does_not_start_when_dry_threshold_not_met();
    rc |= test_idle_starts_when_mad_reached_full_auto();
    rc |= test_full_auto_requires_eta_debt();
    rc |= test_semi_auto_starts_without_etacc();
    rc |= test_stop_on_soil_recovered();
    rc |= test_stop_on_et_satisfied();
    rc |= test_stop_on_max_runtime();
    rc |= test_fault_on_two_invalid_sensors();
    rc |= test_manual_mode_returns_safe_baseline();
    rc |= test_accumulate_et();

    if (rc == 0) printf("\nall zone tests passed.\n");
    return rc;
}

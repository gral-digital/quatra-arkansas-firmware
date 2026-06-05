/**
 * @file test_uart.c
 * @brief Unit tests for the UART JSON formatters and parser.
 *
 * Uses the vendored cJSON in tests/vendor/ (no system-wide dependency).
 *
 * Build:  make test-uart
 * Run:    ./build/test_uart
 */

#include "test_common.h"
#include "quatra_uart.h"
#include "quatra_zone.h"
#include "quatra_config.h"

#include "cJSON.h"

#include <string.h>

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* ----- Formatters ------------------------------------------------------- */

static int test_format_sensor_data(void)
{
    TEST_BEGIN("format_sensor_data");

    quatra_weather_inputs_t w = {
        .temp_c = 28.5f, .humidity_pct = 45.0f, .wind_ms = 2.1f,
        .pressure_kpa = 100.2f, .lux = 65000.0f, .rain_mm_min = 0.0f
    };
    quatra_zone_result_t results[QUATRA_NUM_ZONES] = { 0 };
    results[0].psi[0] = 48.0f; results[0].psi[1] = 60.0f; results[0].psi[2] = 72.0f;
    results[0].psi_avg_cb = 64.0f;
    results[0].state = QUATRA_ZONE_IDLE;

    char buf[1024];
    int n = quatra_uart_format_sensor_data(buf, sizeof(buf), 1718000000U,
                                           &w, results);
    CHECK_TRUE(n > 0);
    CHECK_TRUE(buf[n - 1] == '\n');

    /* Re-parse to validate structure. */
    cJSON *root = cJSON_Parse(buf);
    CHECK_TRUE(root != NULL);
    if (root) {
        cJSON *type = cJSON_GetObjectItem(root, "type");
        CHECK_TRUE(cJSON_IsString(type));
        CHECK_TRUE(strcmp(type->valuestring, "sensor_data") == 0);

        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        cJSON *weather = cJSON_GetObjectItem(payload, "weather");
        CHECK_TRUE(cJSON_IsObject(weather));

        cJSON *zones = cJSON_GetObjectItem(payload, "zones");
        CHECK_EQ(cJSON_GetArraySize(zones), QUATRA_NUM_ZONES);

        cJSON *z0 = cJSON_GetArrayItem(zones, 0);
        cJSON *psi = cJSON_GetObjectItem(z0, "psi");
        CHECK_EQ(cJSON_GetArraySize(psi), QUATRA_SENSORS_PER_ZONE);
        cJSON_Delete(root);
    }
    TEST_END();
    return 0;
}

static int test_format_buffer_too_small(void)
{
    TEST_BEGIN("format_buffer_too_small");
    char tiny[8];
    int n = quatra_uart_format_valve_state(tiny, sizeof(tiny), 0,
                                           0, true, QUATRA_REASON_MAD_TRIGGER,
                                           10.0f, 10.0f);
    CHECK_EQ(n, -1);
    TEST_END();
    return 0;
}

static int test_format_valve_state(void)
{
    TEST_BEGIN("format_valve_state");

    char buf[512];
    int n = quatra_uart_format_valve_state(buf, sizeof(buf), 1718000100U,
                                           3, false, QUATRA_REASON_SOIL_RECOVERED,
                                           14.28f, 0.0f);
    CHECK_TRUE(n > 0);
    CHECK_TRUE(contains(buf, "\"type\":\"valve_state\""));
    CHECK_TRUE(contains(buf, "\"zone_id\":3"));
    CHECK_TRUE(contains(buf, "\"state\":\"CLOSED\""));
    CHECK_TRUE(contains(buf, "\"reason\":\"SOIL_RECOVERED\""));

    TEST_END();
    return 0;
}

static int test_format_et_daily(void)
{
    TEST_BEGIN("format_et_daily");

    quatra_zone_config_t cfg[QUATRA_NUM_ZONES] = { 0 };
    float ETc[QUATRA_NUM_ZONES]   = { 0 };
    float ETacc[QUATRA_NUM_ZONES] = { 0 };
    cfg[0].kc = 0.85f; ETc[0] = 4.76f; ETacc[0] = 14.28f;

    char buf[2048];
    int n = quatra_uart_format_et_daily(buf, sizeof(buf), 1718086799U,
                                        5.6f, 152, cfg, ETc, ETacc);
    CHECK_TRUE(n > 0);
    CHECK_TRUE(contains(buf, "\"day_of_year\":152"));

    /* Re-parse to validate the float (cJSON serialises floats with full
     * precision so a literal "5.6" string match would be brittle). */
    cJSON *root = cJSON_Parse(buf);
    CHECK_TRUE(root != NULL);
    if (root) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        cJSON *et0     = cJSON_GetObjectItem(payload, "ET0_mm");
        CHECK_TRUE(cJSON_IsNumber(et0));
        CHECK_NEAR(et0->valuedouble, 5.6, 1e-3);

        cJSON *zones = cJSON_GetObjectItem(payload, "zones");
        CHECK_EQ(cJSON_GetArraySize(zones), QUATRA_NUM_ZONES);
        cJSON *z0    = cJSON_GetArrayItem(zones, 0);
        cJSON *etacc = cJSON_GetObjectItem(z0, "ETacc_mm");
        CHECK_NEAR(etacc->valuedouble, 14.28, 1e-3);
        cJSON_Delete(root);
    }

    TEST_END();
    return 0;
}

static int test_format_alarm(void)
{
    TEST_BEGIN("format_alarm");
    char buf[512];
    int n = quatra_uart_format_alarm(buf, sizeof(buf), 1718000200U,
                                     "SENSOR_FAULT", 2, 1,
                                     "Open circuit detected on psi_2 zone 2");
    CHECK_TRUE(n > 0);
    CHECK_TRUE(contains(buf, "\"code\":\"SENSOR_FAULT\""));
    CHECK_TRUE(contains(buf, "\"zone_id\":2"));
    CHECK_TRUE(contains(buf, "\"sensor_idx\":1"));
    TEST_END();
    return 0;
}

static int test_format_system_status(void)
{
    TEST_BEGIN("format_system_status");

    quatra_zone_result_t r[QUATRA_NUM_ZONES] = { 0 };
    r[0].state = QUATRA_ZONE_IRRIGATING;
    r[3].state = QUATRA_ZONE_IRRIGATING;

    char buf[1024];
    int n = quatra_uart_format_system_status(buf, sizeof(buf), 1718000000U,
                                             "1.1.0", false, true, 3600, r);
    CHECK_TRUE(n > 0);
    CHECK_TRUE(contains(buf, "\"firmware_version\":\"1.1.0\""));
    CHECK_TRUE(contains(buf, "\"active_zones\":[0,3]"));
    TEST_END();
    return 0;
}

/* ----- Parser ----------------------------------------------------------- */

static int test_parse_set_mode(void)
{
    TEST_BEGIN("parse_set_mode");

    const char *json =
        "{\"type\":\"set_mode\",\"payload\":{\"zone_id\":0,\"mode\":\"MANUAL\"}}";
    quatra_uart_command_t cmd;
    int rc = quatra_uart_parse_command(json, &cmd);
    CHECK_EQ(rc, 0);
    CHECK_EQ(cmd.type, QUATRA_CMD_SET_MODE);
    CHECK_EQ(cmd.mode_zone_id, 0);
    CHECK_EQ(cmd.mode_value, QUATRA_MODE_MANUAL);
    TEST_END();
    return 0;
}

static int test_parse_set_mode_all_zones(void)
{
    TEST_BEGIN("parse_set_mode_all");
    const char *json =
        "{\"type\":\"set_mode\",\"payload\":{\"zone_id\":-1,\"mode\":\"SEMI_AUTO\"}}";
    quatra_uart_command_t cmd;
    CHECK_EQ(quatra_uart_parse_command(json, &cmd), 0);
    CHECK_EQ(cmd.mode_zone_id, -1);
    CHECK_EQ(cmd.mode_value, QUATRA_MODE_SEMI_AUTO);
    TEST_END();
    return 0;
}

static int test_parse_manual_valve(void)
{
    TEST_BEGIN("parse_manual_valve");
    const char *json =
        "{\"type\":\"manual_valve\",\"payload\":{\"zone_id\":2,\"state\":\"OPEN\"}}";
    quatra_uart_command_t cmd;
    CHECK_EQ(quatra_uart_parse_command(json, &cmd), 0);
    CHECK_EQ(cmd.type, QUATRA_CMD_MANUAL_VALVE);
    CHECK_EQ(cmd.manual_zone_id, 2);
    CHECK_TRUE(cmd.manual_open);
    TEST_END();
    return 0;
}

static int test_parse_set_params(void)
{
    TEST_BEGIN("parse_set_params");
    const char *json =
        "{\"type\":\"set_params\",\"payload\":{"
        "\"global\":{\"latitude_deg\":33.57,\"h_anemometer_m\":2.0},"
        "\"zones\":["
        "{\"id\":0,\"kc\":0.85,\"mad_cb\":55.0,\"weights\":[1,2,3],"
        "\"area_m2\":1200,\"flow_m3h\":18.0}"
        "]}}";

    quatra_uart_command_t cmd;
    CHECK_EQ(quatra_uart_parse_command(json, &cmd), 0);
    CHECK_EQ(cmd.type, QUATRA_CMD_SET_PARAMS);
    CHECK_TRUE(cmd.setp_mask & QUATRA_SETPARAM_MASK_LATITUDE);
    CHECK_TRUE(cmd.setp_mask & QUATRA_SETPARAM_MASK_HANEM);
    CHECK_NEAR(cmd.setp_latitude_rad, 33.57 * 0.017453292519943295, 1e-4);
    CHECK_TRUE(cmd.setp_zone_mask & (1U << 0));
    CHECK_NEAR(cmd.setp_zones[0].kc, 0.85, 1e-3);
    CHECK_NEAR(cmd.setp_zones[0].mad_cb, 55.0, 1e-3);
    CHECK_NEAR(cmd.setp_zones[0].area_m2, 1200.0, 1e-3);
    CHECK_NEAR(cmd.setp_zones[0].weights[2], 3.0, 1e-3);
    TEST_END();
    return 0;
}

static int test_parse_request_status(void)
{
    TEST_BEGIN("parse_request_status");
    const char *json = "{\"type\":\"request_status\",\"payload\":{}}";
    quatra_uart_command_t cmd;
    CHECK_EQ(quatra_uart_parse_command(json, &cmd), 0);
    CHECK_EQ(cmd.type, QUATRA_CMD_REQUEST_STATUS);
    TEST_END();
    return 0;
}

static int test_parse_reset_fault_and_etacc(void)
{
    TEST_BEGIN("parse_reset_fault_etacc");
    quatra_uart_command_t cmd;
    CHECK_EQ(quatra_uart_parse_command(
        "{\"type\":\"reset_fault\",\"payload\":{\"zone_id\":2}}", &cmd), 0);
    CHECK_EQ(cmd.type, QUATRA_CMD_RESET_FAULT);
    CHECK_EQ(cmd.target_zone_id, 2);

    CHECK_EQ(quatra_uart_parse_command(
        "{\"type\":\"reset_ETacc\",\"payload\":{\"zone_id\":5}}", &cmd), 0);
    CHECK_EQ(cmd.type, QUATRA_CMD_RESET_ETACC);
    CHECK_EQ(cmd.target_zone_id, 5);
    TEST_END();
    return 0;
}

static int test_parse_malformed(void)
{
    TEST_BEGIN("parse_malformed");
    quatra_uart_command_t cmd;
    CHECK_EQ(quatra_uart_parse_command("not json at all", &cmd), -1);
    CHECK_EQ(quatra_uart_parse_command("{\"type\":42}", &cmd), -1);
    CHECK_EQ(quatra_uart_parse_command(
        "{\"type\":\"bogus_cmd\",\"payload\":{}}", &cmd), -1);
    TEST_END();
    return 0;
}

int main(void)
{
    int rc = 0;
    rc |= test_format_sensor_data();
    rc |= test_format_buffer_too_small();
    rc |= test_format_valve_state();
    rc |= test_format_et_daily();
    rc |= test_format_alarm();
    rc |= test_format_system_status();

    rc |= test_parse_set_mode();
    rc |= test_parse_set_mode_all_zones();
    rc |= test_parse_manual_valve();
    rc |= test_parse_set_params();
    rc |= test_parse_request_status();
    rc |= test_parse_reset_fault_and_etacc();
    rc |= test_parse_malformed();

    if (rc == 0) printf("\nall UART tests passed.\n");
    return rc;
}

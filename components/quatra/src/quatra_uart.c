/**
 * @file    quatra_uart.c
 * @brief   JSON payload (de)serialization for the webapp link.
 * @author  Quatra Engineering
 * @date    2026-06
 * @version 1.1.0
 *
 * Per PRD v1.1 amendments §2 every function in this file is a pure string
 * operation. No UART driver calls, no hardware. The integrator writes the
 * resulting bytes to its own transport.
 */

#include "quatra_uart.h"

#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Enum → string                                                             */
/* ------------------------------------------------------------------------- */

const char *quatra_uart_state_str(quatra_zone_state_t s)
{
    switch (s) {
        case QUATRA_ZONE_IDLE:        return "IDLE";
        case QUATRA_ZONE_IRRIGATING:  return "IRRIGATING";
        case QUATRA_ZONE_FAULT:       return "FAULT";
        default:                      return "UNKNOWN";
    }
}

const char *quatra_uart_reason_str(quatra_valve_reason_t r)
{
    switch (r) {
        case QUATRA_REASON_MAD_TRIGGER:    return "MAD_TRIGGER";
        case QUATRA_REASON_SOIL_RECOVERED: return "SOIL_RECOVERED";
        case QUATRA_REASON_ET_SATISFIED:   return "ET_SATISFIED";
        case QUATRA_REASON_MAX_RUNTIME:    return "MAX_RUNTIME";
        case QUATRA_REASON_MANUAL_CMD:     return "MANUAL_CMD";
        case QUATRA_REASON_FAULT:          return "FAULT";
        default:                           return "NONE";
    }
}

const char *quatra_uart_mode_str(quatra_op_mode_t m)
{
    switch (m) {
        case QUATRA_MODE_MANUAL:    return "MANUAL";
        case QUATRA_MODE_SEMI_AUTO: return "SEMI_AUTO";
        case QUATRA_MODE_FULL_AUTO: return "FULL_AUTO";
        default:                    return "UNKNOWN";
    }
}

static quatra_op_mode_t parse_mode(const char *s)
{
    if (s == NULL) return QUATRA_MODE_FULL_AUTO;
    if (strcmp(s, "MANUAL")    == 0) return QUATRA_MODE_MANUAL;
    if (strcmp(s, "SEMI_AUTO") == 0) return QUATRA_MODE_SEMI_AUTO;
    return QUATRA_MODE_FULL_AUTO;
}

/* ------------------------------------------------------------------------- */
/* Internal: render a cJSON tree to the caller's buffer + newline.           */
/* ------------------------------------------------------------------------- */

static int render(cJSON *root, char *buf, size_t buf_len)
{
    if (root == NULL || buf == NULL || buf_len == 0) {
        if (root) cJSON_Delete(root);
        return -1;
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s == NULL) return -1;

    size_t len = strlen(s);
    if (len + 2 > buf_len) {            /* +1 for '\n', +1 for terminator. */
        cJSON_free(s);
        return -1;
    }
    memcpy(buf, s, len);
    buf[len]     = '\n';
    buf[len + 1] = '\0';
    cJSON_free(s);
    return (int)(len + 1);
}

static cJSON *frame_skeleton(const char *type, uint32_t ts_epoch, cJSON **payload_out)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return NULL;
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "ts",   (double)ts_epoch);
    cJSON *payload = cJSON_AddObjectToObject(root, "payload");
    if (payload == NULL) { cJSON_Delete(root); return NULL; }
    if (payload_out) *payload_out = payload;
    return root;
}

/* ------------------------------------------------------------------------- */
/* Formatters                                                                */
/* ------------------------------------------------------------------------- */

int quatra_uart_format_sensor_data(char *buf, size_t buf_len,
                                   uint32_t ts_epoch,
                                   const quatra_weather_inputs_t *w,
                                   const quatra_zone_result_t results[QUATRA_NUM_ZONES])
{
    if (w == NULL || results == NULL) return -1;

    cJSON *payload = NULL;
    cJSON *root = frame_skeleton("sensor_data", ts_epoch, &payload);
    if (root == NULL) return -1;

    cJSON *weather = cJSON_AddObjectToObject(payload, "weather");
    cJSON_AddNumberToObject(weather, "temp_c",       w->temp_c);
    cJSON_AddNumberToObject(weather, "humidity_pct", w->humidity_pct);
    cJSON_AddNumberToObject(weather, "wind_ms",      w->wind_ms);
    cJSON_AddNumberToObject(weather, "pressure_kpa", w->pressure_kpa);
    cJSON_AddNumberToObject(weather, "lux",          w->lux);
    cJSON_AddNumberToObject(weather, "rain_mm_min", w->rain_mm_min);

    cJSON *zones = cJSON_AddArrayToObject(payload, "zones");
    for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z) {
        cJSON *zo = cJSON_CreateObject();
        cJSON_AddNumberToObject(zo, "id", z);
        cJSON *psi = cJSON_AddArrayToObject(zo, "psi");
        for (int i = 0; i < QUATRA_SENSORS_PER_ZONE; ++i) {
            cJSON_AddItemToArray(psi, cJSON_CreateNumber(results[z].psi[i]));
        }
        cJSON_AddNumberToObject(zo, "psi_avg", results[z].psi_avg_cb);
        cJSON_AddStringToObject(zo, "state",   quatra_uart_state_str(results[z].state));
        cJSON_AddItemToArray(zones, zo);
    }
    return render(root, buf, buf_len);
}

int quatra_uart_format_valve_state(char *buf, size_t buf_len,
                                   uint32_t ts_epoch,
                                   uint8_t  zone_id,
                                   bool     open,
                                   quatra_valve_reason_t reason,
                                   float    D_target_mm,
                                   float    ETacc_mm)
{
    cJSON *payload = NULL;
    cJSON *root = frame_skeleton("valve_state", ts_epoch, &payload);
    if (root == NULL) return -1;
    cJSON_AddNumberToObject(payload, "zone_id",     zone_id);
    cJSON_AddStringToObject(payload, "state",       open ? "OPEN" : "CLOSED");
    cJSON_AddStringToObject(payload, "reason",      quatra_uart_reason_str(reason));
    cJSON_AddNumberToObject(payload, "D_target_mm", D_target_mm);
    cJSON_AddNumberToObject(payload, "ETacc_mm",    ETacc_mm);
    return render(root, buf, buf_len);
}

int quatra_uart_format_et_daily(char *buf, size_t buf_len,
                                uint32_t ts_epoch,
                                float    ET0_mm,
                                uint16_t day_of_year,
                                const quatra_zone_config_t cfg[QUATRA_NUM_ZONES],
                                const float ETc_mm[QUATRA_NUM_ZONES],
                                const float ETacc_mm[QUATRA_NUM_ZONES])
{
    if (cfg == NULL || ETc_mm == NULL || ETacc_mm == NULL) return -1;
    cJSON *payload = NULL;
    cJSON *root = frame_skeleton("et_daily", ts_epoch, &payload);
    if (root == NULL) return -1;

    cJSON_AddNumberToObject(payload, "ET0_mm",      ET0_mm);
    cJSON_AddNumberToObject(payload, "day_of_year", day_of_year);

    cJSON *zones = cJSON_AddArrayToObject(payload, "zones");
    for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z) {
        cJSON *zo = cJSON_CreateObject();
        cJSON_AddNumberToObject(zo, "id",       z);
        cJSON_AddNumberToObject(zo, "Kc",       cfg[z].kc);
        cJSON_AddNumberToObject(zo, "ETc_mm",   ETc_mm[z]);
        cJSON_AddNumberToObject(zo, "ETacc_mm", ETacc_mm[z]);
        cJSON_AddItemToArray(zones, zo);
    }
    return render(root, buf, buf_len);
}

int quatra_uart_format_alarm(char *buf, size_t buf_len,
                             uint32_t ts_epoch,
                             const char *code,
                             int8_t      zone_id,
                             int8_t      sensor_idx,
                             const char *detail)
{
    if (code == NULL) return -1;
    cJSON *payload = NULL;
    cJSON *root = frame_skeleton("alarm", ts_epoch, &payload);
    if (root == NULL) return -1;
    cJSON_AddStringToObject(payload, "code", code);
    if (zone_id    >= 0) cJSON_AddNumberToObject(payload, "zone_id",    zone_id);
    if (sensor_idx >= 0) cJSON_AddNumberToObject(payload, "sensor_idx", sensor_idx);
    if (detail)          cJSON_AddStringToObject(payload, "detail",     detail);
    return render(root, buf, buf_len);
}

int quatra_uart_format_system_status(char *buf, size_t buf_len,
                                     uint32_t ts_epoch,
                                     const char *firmware_version,
                                     bool wifi_connected,
                                     bool rtc_sync,
                                     uint32_t uptime_s,
                                     const quatra_zone_result_t results[QUATRA_NUM_ZONES])
{
    if (firmware_version == NULL || results == NULL) return -1;
    cJSON *payload = NULL;
    cJSON *root = frame_skeleton("system_status", ts_epoch, &payload);
    if (root == NULL) return -1;
    cJSON_AddStringToObject(payload, "firmware_version", firmware_version);
    cJSON_AddBoolToObject  (payload, "wifi_connected",   wifi_connected);
    cJSON_AddBoolToObject  (payload, "rtc_sync",         rtc_sync);
    cJSON_AddNumberToObject(payload, "uptime_s",         uptime_s);

    cJSON *active = cJSON_AddArrayToObject(payload, "active_zones");
    for (uint8_t z = 0; z < QUATRA_NUM_ZONES; ++z) {
        if (results[z].state == QUATRA_ZONE_IRRIGATING) {
            cJSON_AddItemToArray(active, cJSON_CreateNumber(z));
        }
    }
    return render(root, buf, buf_len);
}

/* ------------------------------------------------------------------------- */
/* Parser                                                                    */
/* ------------------------------------------------------------------------- */

static bool parse_set_mode(cJSON *payload, quatra_uart_command_t *cmd)
{
    cJSON *zone = cJSON_GetObjectItem(payload, "zone_id");
    cJSON *mode = cJSON_GetObjectItem(payload, "mode");
    if (zone == NULL || !cJSON_IsString(mode)) return false;
    cmd->type          = QUATRA_CMD_SET_MODE;
    cmd->mode_zone_id  = (int8_t)cJSON_GetNumberValue(zone);
    cmd->mode_value    = parse_mode(mode->valuestring);
    return true;
}

static bool parse_manual_valve(cJSON *payload, quatra_uart_command_t *cmd)
{
    cJSON *zone  = cJSON_GetObjectItem(payload, "zone_id");
    cJSON *state = cJSON_GetObjectItem(payload, "state");
    if (zone == NULL || !cJSON_IsString(state)) return false;
    cmd->type            = QUATRA_CMD_MANUAL_VALVE;
    cmd->manual_zone_id  = (uint8_t)cJSON_GetNumberValue(zone);
    cmd->manual_open     = (strcmp(state->valuestring, "OPEN") == 0);
    return true;
}

static bool parse_set_params(cJSON *payload, quatra_uart_command_t *cmd)
{
    cmd->type            = QUATRA_CMD_SET_PARAMS;
    cmd->setp_mask       = 0;
    cmd->setp_zone_mask  = 0;
    memset(cmd->setp_zones, 0, sizeof(cmd->setp_zones));

    cJSON *global = cJSON_GetObjectItem(payload, "global");
    if (cJSON_IsObject(global)) {
        cJSON *lat   = cJSON_GetObjectItem(global, "latitude_deg");
        cJSON *hanem = cJSON_GetObjectItem(global, "h_anemometer_m");
        if (cJSON_IsNumber(lat)) {
            cmd->setp_latitude_rad =
                (float)(cJSON_GetNumberValue(lat) * 0.017453292519943295);
            cmd->setp_mask |= QUATRA_SETPARAM_MASK_LATITUDE;
        }
        if (cJSON_IsNumber(hanem)) {
            cmd->setp_h_anemometer_m = (float)cJSON_GetNumberValue(hanem);
            cmd->setp_mask          |= QUATRA_SETPARAM_MASK_HANEM;
        }
    }

    cJSON *zones = cJSON_GetObjectItem(payload, "zones");
    if (cJSON_IsArray(zones)) {
        cJSON *z;
        cJSON_ArrayForEach(z, zones) {
            cJSON *idn = cJSON_GetObjectItem(z, "id");
            if (!cJSON_IsNumber(idn)) continue;
            int idx = (int)cJSON_GetNumberValue(idn);
            if (idx < 0 || idx >= QUATRA_NUM_ZONES) continue;

            quatra_zone_config_t *zc = &cmd->setp_zones[idx];
            cJSON *v;
            v = cJSON_GetObjectItem(z, "kc");       if (cJSON_IsNumber(v)) zc->kc       = (float)v->valuedouble;
            v = cJSON_GetObjectItem(z, "mad_cb");   if (cJSON_IsNumber(v)) zc->mad_cb   = (float)v->valuedouble;
            v = cJSON_GetObjectItem(z, "area_m2");  if (cJSON_IsNumber(v)) zc->area_m2  = (float)v->valuedouble;
            v = cJSON_GetObjectItem(z, "flow_m3h"); if (cJSON_IsNumber(v)) zc->flow_m3h = (float)v->valuedouble;
            cJSON *weights = cJSON_GetObjectItem(z, "weights");
            if (cJSON_IsArray(weights) &&
                cJSON_GetArraySize(weights) == QUATRA_SENSORS_PER_ZONE) {
                for (int i = 0; i < QUATRA_SENSORS_PER_ZONE; ++i) {
                    cJSON *wv = cJSON_GetArrayItem(weights, i);
                    if (cJSON_IsNumber(wv)) zc->weights[i] = (float)wv->valuedouble;
                }
            }
            cmd->setp_zone_mask |= (1U << idx);
        }
    }
    return true;
}

int quatra_uart_parse_command(const char *json, quatra_uart_command_t *cmd_out)
{
    if (json == NULL || cmd_out == NULL) return -1;
    memset(cmd_out, 0, sizeof(*cmd_out));

    cJSON *root = cJSON_Parse(json);
    if (root == NULL) return -1;

    cJSON *type    = cJSON_GetObjectItem(root, "type");
    cJSON *payload = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsString(type)) { cJSON_Delete(root); return -1; }

    int rc = 0;
    const char *t = type->valuestring;

    if (strcmp(t, "set_params") == 0) {
        if (!parse_set_params(payload, cmd_out)) rc = -1;
    } else if (strcmp(t, "set_mode") == 0) {
        if (!parse_set_mode(payload, cmd_out)) rc = -1;
    } else if (strcmp(t, "manual_valve") == 0) {
        if (!parse_manual_valve(payload, cmd_out)) rc = -1;
    } else if (strcmp(t, "request_status") == 0) {
        cmd_out->type = QUATRA_CMD_REQUEST_STATUS;
    } else if (strcmp(t, "reset_fault") == 0) {
        cJSON *zone = cJSON_GetObjectItem(payload, "zone_id");
        if (!cJSON_IsNumber(zone)) { rc = -1; }
        else {
            cmd_out->type           = QUATRA_CMD_RESET_FAULT;
            cmd_out->target_zone_id = (uint8_t)zone->valuedouble;
        }
    } else if (strcmp(t, "reset_ETacc") == 0) {
        cJSON *zone = cJSON_GetObjectItem(payload, "zone_id");
        if (!cJSON_IsNumber(zone)) { rc = -1; }
        else {
            cmd_out->type           = QUATRA_CMD_RESET_ETACC;
            cmd_out->target_zone_id = (uint8_t)zone->valuedouble;
        }
    } else {
        rc = -1;
    }

    cJSON_Delete(root);
    return rc;
}

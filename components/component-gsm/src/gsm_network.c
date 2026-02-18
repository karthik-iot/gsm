/*
 * gsm_network.c - APN, PDP context, data attachment
 * SPDX-License-Identifier: MIT
 */

#include "gsm_private.h"

bool gsm_set_apn(gsm_handle_t m, const char *apn)
{
    /* Deactivate any active PDP contexts first */
    gsm_flush_input(m);
    gsm_uart_writeln(m, "AT+QIACT?");
    char resp[256];
    gsm_read_response(m, resp, sizeof(resp), 2000);

    if (strstr(resp, "+QIACT:")) {
        ESP_LOGD(GSM_TAG, "PDP active, deactivating...");
        gsm_send_at(m, "AT+QIDEACT=1", "OK", 40000);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* Set APN */
    gsm_flush_input(m);
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", apn);

    if (gsm_send_at(m, cmd, "OK", 2000)) {
        ESP_LOGD(GSM_TAG, "APN set: %s", apn);
        return true;
    }

    /* If "Operation not allowed", check if already configured */
    gsm_flush_input(m);
    gsm_uart_writeln(m, "AT+CGDCONT?");
    char query[256];
    gsm_read_response(m, query, sizeof(query), 2000);

    if (strstr(query, apn)) {
        ESP_LOGD(GSM_TAG, "APN already configured");
        return true;
    }

    ESP_LOGE(GSM_TAG, "APN config failed");
    return false;
}

bool gsm_wait_for_network(gsm_handle_t m, uint32_t timeout_ms)
{
    int64_t start = esp_timer_get_time() / 1000;
    while ((esp_timer_get_time() / 1000 - start) < timeout_ms) {
        int status = gsm_get_registration_status(m, true);
        if (status == 1 || status == 5) {
            ESP_LOGI(GSM_TAG, "Network registered (status=%d)", status);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGE(GSM_TAG, "Network registration timeout");
    return false;
}

bool gsm_attach_data(gsm_handle_t m, const char *apn, const char *user, const char *pass, int auth)
{
    if (!user) user = "";
    if (!pass) pass = "";

    ESP_LOGI(GSM_TAG, "Attaching data...");

    /* Check GPRS attachment */
    gsm_flush_input(m);
    gsm_uart_writeln(m, "AT+CGATT?");
    char resp[128];
    gsm_read_response(m, resp, sizeof(resp), 2000);

    if (strstr(resp, "+CGATT: 0")) {
        ESP_LOGD(GSM_TAG, "GPRS not attached, attaching...");
        if (!gsm_send_at(m, "AT+CGATT=1", "OK", 10000)) {
            ESP_LOGE(GSM_TAG, "GPRS attach failed");
            m->last_error = GSM_ERR_GPRS_NOT_ATTACHED;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* Set APN */
    if (!gsm_set_apn(m, apn)) {
        m->last_error = GSM_ERR_APN_CONFIG;
        return false;
    }

    /* Configure authentication if provided */
    if (strlen(user) > 0) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "AT+QICSGP=1,1,\"%s\",\"%s\",\"%s\",%d", apn, user, pass, auth);

        gsm_flush_input(m);
        gsm_uart_writeln(m, cmd);
        char auth_resp[128];
        gsm_read_response(m, auth_resp, sizeof(auth_resp), 2000);

        if (!strstr(auth_resp, "OK") && !strstr(auth_resp, "Operation not allowed")) {
            ESP_LOGE(GSM_TAG, "Auth config failed");
            m->last_error = GSM_ERR_AUTH_CONFIG;
            return false;
        }
    }

    ESP_LOGI(GSM_TAG, "Data attach OK");
    return true;
}

bool gsm_activate_pdp(gsm_handle_t m, int ctx_id)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+QIACT=%d", ctx_id);
    return gsm_send_at(m, cmd, "OK", 15000);
}

bool gsm_deactivate_pdp(gsm_handle_t m, int ctx_id)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+QIDEACT=%d", ctx_id);
    return gsm_send_at(m, cmd, "OK", 15000);
}

bool gsm_configure_context(gsm_handle_t m, int ctx_id, int type, const char *apn,
                           const char *user, const char *pass, int auth)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+QICSGP=%d,%d,\"%s\",\"%s\",\"%s\",%d",
             ctx_id, type, apn, user ? user : "", pass ? pass : "", auth);
    return gsm_send_at(m, cmd, "OK", 2000);
}

/* ── NTP ──────────────────────────────────────────────────────────── */

bool gsm_ntp_sync(gsm_handle_t m, const char *server, int timezone, int ctx_id)
{
    if (!server || !server[0]) return false;
    if (timezone < -48 || timezone > 56) return false;

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+QNTP=%d,\"%s\",123,%d", ctx_id, server, timezone);

    if (!gsm_send_at(m, cmd, "OK", 1000)) return false;
    return gsm_expect_urc(m, "+QNTP: 0", 125000);
}

/* ── DNS ──────────────────────────────────────────────────────────── */

bool gsm_set_dns(gsm_handle_t m, const char *primary, const char *secondary, int ctx_id)
{
    char cmd[128];
    if (primary && primary[0]) {
        if (secondary && secondary[0]) {
            snprintf(cmd, sizeof(cmd), "AT+QIDNSCFG=%d,\"%s\",\"%s\"", ctx_id, primary, secondary);
        } else {
            snprintf(cmd, sizeof(cmd), "AT+QIDNSCFG=%d,\"%s\"", ctx_id, primary);
        }
    } else {
        snprintf(cmd, sizeof(cmd), "AT+QIDNSCFG=%d", ctx_id);
    }
    return gsm_send_at(m, cmd, "OK", 1000);
}

/* ── Ping ─────────────────────────────────────────────────────────── */

bool gsm_ping(gsm_handle_t m, const char *host, int ctx_id, int timeout_s, int count)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+QPING=%d,\"%s\",%d,%d", ctx_id, host, timeout_s, count);

    gsm_flush_input(m);
    gsm_uart_writeln(m, cmd);

    char ack[256];
    gsm_read_response(m, ack, sizeof(ack), 2000);
    if (!strstr(ack, "OK")) return false;

    uint32_t wait_ms = (timeout_s * 1000 * count) + 5000;
    char report[512];
    int n = gsm_collect_response(m, report, sizeof(report), wait_ms);

    return (n > 0 && strstr(report, "+QPING:") && !strstr(report, "ERROR"));
}

/* ── SSL/TLS ──────────────────────────────────────────────────────── */

bool gsm_ssl_configure(gsm_handle_t m, int ctx_id, const char *ca_path, bool verify)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"cacert\",%d,\"%s\"", ctx_id, ca_path);
    if (!gsm_send_at(m, cmd, "OK", 1000)) return false;

    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"seclevel\",%d,%d", ctx_id, verify ? 2 : 0);
    return gsm_send_at(m, cmd, "OK", 1000);
}

bool gsm_ssl_upload_cert(gsm_handle_t m, const char *cert, const char *path)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+QFUPL=\"%s\",%d,100", path, (int)strlen(cert));
    if (!gsm_send_at(m, cmd, "CONNECT", 3000)) return false;

    gsm_uart_write(m, cert, strlen(cert));

    char resp[128];
    gsm_read_response(m, resp, sizeof(resp), 5000);
    return strstr(resp, "OK") != NULL;
}

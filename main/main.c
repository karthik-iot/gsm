#include <stdio.h>
#include <string.h>
#include "gsm_modem.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#define TAG "gsm-test"

/* ── Hardware config ─────────────────────────────────────────────── */
#define UART_TX_PIN    12
#define UART_RX_PIN    36
#define UART_BAUD_RATE 115200

/* ── Network config ──────────────────────────────────────────────── */
#define GSM_APN        "jionet"
#define GSM_APN_USER   ""
#define GSM_APN_PASS   ""
#define GSM_APN_AUTH   0          /* 0=none, 1=PAP, 2=CHAP */

/* ── Test mode: uncomment to run WSS test instead of HTTPS POST ─── */
#define TEST_WSS

/* ── HTTPS test endpoint ─────────────────────────────────────────── */
#define HTTPS_URL          "https://rbaskets.in/GSM"
// #define HTTPS_URL          "https://demo.iotready.co/api/method/otp.api.insert_iot_event"
#define HTTPS_URL_FALLBACK "https://httpbin.org/post"

/* ── WSS test endpoint ──────────────────────────────────────────── */
#define WS_HOST "wstest.iotready.com"
#define WS_PORT 443
#define WS_PATH "/ws"

/* ── WSS CA cert (ISRG Root X1 — Let's Encrypt trust anchor, valid
 * until 2035-06-04) — uploaded to the modem and used to verify
 * wstest.iotready.com's server certificate during the TLS handshake. */
static const char *WS_CA_CERT =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n";

static gsm_handle_t modem = NULL;

/* ── Step 1: Initialize modem UART ───────────────────────────────── */
static bool step_init_modem(void)
{
    ESP_LOGI(TAG, "──── Step 1: Init UART & modem handle ────");

    gsm_config_t cfg = GSM_CONFIG_DEFAULT();
    cfg.uart_port = UART_NUM_1;
    cfg.tx_pin    = UART_TX_PIN;
    cfg.rx_pin    = UART_RX_PIN;
    cfg.baud_rate = UART_BAUD_RATE;

    esp_err_t err = gsm_init(&cfg, &modem);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gsm_init failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "UART initialized OK");
    return true;
}

/* ── Step 2: Sync with modem (AT handshake) ──────────────────────── */
static bool step_begin_modem(void)
{
    ESP_LOGI(TAG, "──── Step 2: Sync with modem (gsm_begin) ────");

    for (int attempt = 1; attempt <= 10; attempt++) {
        ESP_LOGI(TAG, "  Attempt %d/10...", attempt);
        gsm_err_t gerr = gsm_begin(modem);
        if (gerr == GSM_OK) {
            ESP_LOGI(TAG, "Modem ready!");
            return true;
        }
        ESP_LOGW(TAG, "  gsm_begin returned %d, retrying...", gerr);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGE(TAG, "Modem did not respond after 10 attempts");
    return false;
}

/* ── Step 3: Read modem diagnostics ──────────────────────────────── */
static void step_diagnostics(void)
{
    ESP_LOGI(TAG, "──── Step 3: Modem diagnostics ────");

    /* IMEI */
    char imei[20] = {0};
    if (gsm_get_imei(modem, imei, sizeof(imei))) {
        ESP_LOGI(TAG, "  IMEI: %s", imei);
    } else {
        ESP_LOGW(TAG, "  IMEI: failed to read");
    }

    /* SIM */
    if (gsm_is_sim_ready(modem)) {
        ESP_LOGI(TAG, "  SIM: Ready");
    } else {
        ESP_LOGW(TAG, "  SIM: Not ready");
    }

    /* SIM number (MSISDN) */
    char sim_num[32] = {0};
    if (gsm_get_sim_number(modem, sim_num, sizeof(sim_num))) {
        ESP_LOGI(TAG, "  SIM Number: %s", sim_num);
    } else {
        ESP_LOGW(TAG, "  SIM Number: not available (normal for many carriers)");
    }

    /* Signal strength */
    int csq = gsm_get_signal_strength(modem);
    if (csq >= 0) {
        ESP_LOGI(TAG, "  Signal (CSQ): %d/31", csq);
    } else {
        ESP_LOGW(TAG, "  Signal: failed to read");
    }

    /* Operator */
    char oper[64] = {0};
    if (gsm_get_operator(modem, oper, sizeof(oper))) {
        ESP_LOGI(TAG, "  Operator: %s", oper);
    } else {
        ESP_LOGW(TAG, "  Operator: not registered yet");
    }
}

/* ── Step 4: Wait for network registration ───────────────────────── */
static bool step_wait_network(void)
{
    ESP_LOGI(TAG, "──── Step 4: Wait for network registration ────");

    if (gsm_wait_for_network(modem, 60000)) {
        ESP_LOGI(TAG, "Network registered!");
        return true;
    }

    ESP_LOGE(TAG, "Network registration timed out (60s)");
    return false;
}

/* ── Step 5: Attach GPRS data + set APN ──────────────────────────── */
static bool step_attach_data(void)
{
    /* Try to read the APN already configured in the modem */
    char apn[64] = {0};
    if (gsm_get_apn(modem, apn, sizeof(apn)) && apn[0]) {
        ESP_LOGI(TAG, "──── Step 5: Attach GPRS data (APN from modem: %s) ────", apn);
    } else {
        strncpy(apn, GSM_APN, sizeof(apn) - 1);
        ESP_LOGI(TAG, "──── Step 5: Attach GPRS data (APN fallback: %s) ────", apn);
    }

    if (gsm_attach_data(modem, apn, GSM_APN_USER, GSM_APN_PASS, GSM_APN_AUTH)) {
        ESP_LOGI(TAG, "GPRS attached OK");
        return true;
    }

    ESP_LOGE(TAG, "GPRS attach failed");
    return false;
}

/* ── Step 6: Activate PDP context ────────────────────────────────── */
static bool step_activate_pdp(void)
{
    ESP_LOGI(TAG, "──── Step 6: Activate PDP context ────");

    /* Configure context 1 with APN (auto-detect or fallback) */
    char apn[64] = {0};
    if (!gsm_get_apn(modem, apn, sizeof(apn)) || !apn[0]) {
        strncpy(apn, GSM_APN, sizeof(apn) - 1);
    }
    gsm_configure_context(modem, 1, 1, apn, GSM_APN_USER, GSM_APN_PASS, GSM_APN_AUTH);

    if (gsm_activate_pdp(modem, 1)) {
        ESP_LOGI(TAG, "PDP context 1 activated!");
        return true;
    }

    /* PDP might already be active — check before failing */
    ESP_LOGW(TAG, "PDP activate returned error, checking if already active...");
    char resp[256] = {0};
    gsm_send_at_raw(modem, "AT+QIACT?");
    int n = gsm_read_response(modem, resp, sizeof(resp), 2000);
    if (n > 0 && strstr(resp, "+QIACT: 1")) {
        ESP_LOGI(TAG, "PDP context 1 already active");
        return true;
    }

    ESP_LOGE(TAG, "PDP context activation failed");
    return false;
}

/* ── Step 6b: Upload CA cert for WSS TLS verification ────────────── */
#ifdef TEST_WSS
static bool step_upload_ca_cert(void)
{
    ESP_LOGI(TAG, "──── Step 6b: Upload CA cert for WSS ────");
    if (!gsm_ssl_upload_cert(modem, WS_CA_CERT, "cacert.pem")) {
        ESP_LOGE(TAG, "CA cert upload failed");
        return false;
    }
    ESP_LOGI(TAG, "CA cert uploaded OK");
    return true;
}
#endif

/* ── Step 7: HTTPS POST stress test (only when TEST_WSS is not set) ── */
#ifndef TEST_WSS

/* ── Stress test payload sizes (bytes) ───────────────────────────── */
static const int payload_sizes[] = { 100, 256, 512, 1024, 2048, 4096, 8192, 16384 };
#define NUM_TESTS (sizeof(payload_sizes) / sizeof(payload_sizes[0]))

/**
 * Build a JSON payload of approximately `target_size` bytes:
 * {"test":"stress","seq":<n>,"size":<s>,"data":"AAAA..."}
 */
static char *build_json_payload(int seq, int target_size)
{
    char *buf = malloc(target_size + 1);
    if (!buf) return NULL;

    /* Write the JSON envelope, leave room for the "data" field */
    int header_len = snprintf(buf, target_size + 1,
        "{\"test\":\"stress\",\"seq\":%d,\"size\":%d,\"data\":\"", seq, target_size);

    /* 2 chars for closing "} */
    int pad_len = target_size - header_len - 2;
    if (pad_len < 0) pad_len = 0;

    memset(buf + header_len, 'A', pad_len);
    buf[header_len + pad_len] = '"';
    buf[header_len + pad_len + 1] = '}';
    buf[header_len + pad_len + 2] = '\0';

    return buf;
}

/* Results for summary table */
typedef struct {
    int    size;
    int    time_s;
    bool   ok;
    int    err_code;
} test_result_t;

static test_result_t results[NUM_TESTS];

static void step_stress_test(void)
{
    ESP_LOGI(TAG, "──── Step 7: HTTPS POST Stress Test ────");
    ESP_LOGI(TAG, "  Endpoint: %s", HTTPS_URL);
    ESP_LOGI(TAG, "  Tests: %d payloads (100B → 8KB)\n", (int)NUM_TESTS);

    char response[512] = {0};

    for (int i = 0; i < (int)NUM_TESTS; i++) {
        int size = payload_sizes[i];

        ESP_LOGI(TAG, "━━━ Test %d/%d — Payload: %d bytes ━━━", i + 1, (int)NUM_TESTS, size);

        char *body = build_json_payload(i + 1, size);
        if (!body) {
            ESP_LOGE(TAG, "  malloc failed for %d bytes", size);
            results[i] = (test_result_t){ .size = size, .time_s = 0, .ok = false, .err_code = -99 };
            continue;
        }

        memset(response, 0, sizeof(response));

        int64_t start_us = esp_timer_get_time();

        const char *headers[] = {
            "Authorization: token 85edb3505942b22:849382fae8e94e2",
            "Content-Type: application/json"
        };
        size_t header_count = sizeof(headers) / sizeof(headers[0]);

        gsm_err_t err;
        if (strncmp(HTTPS_URL, "https://", 8) == 0) {
            err = gsm_https_post(modem, HTTPS_URL, body,
                                 response, sizeof(response), headers, header_count);
        } else {
            err = gsm_http_post(modem, HTTPS_URL, body,
                                response, sizeof(response), headers, header_count);
        }

        int elapsed_s = (int)((esp_timer_get_time() - start_us) / 1000000);

        free(body);

        results[i] = (test_result_t){ .size = size, .time_s = elapsed_s, .ok = (err == GSM_OK), .err_code = err };

        if (err == GSM_OK) {
            ESP_LOGI(TAG, "  OK  %d bytes in %d s", size, elapsed_s);
        } else {
            ESP_LOGE(TAG, "  FAIL  %d bytes — error %d in %d s", size, err, elapsed_s);
        }

        /* Brief pause between tests */
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    /* ── Summary table ────────────────────────────────────────────── */
    ESP_LOGI(TAG, "\n╔══════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║       HTTPS POST Stress Test Results     ║");
    ESP_LOGI(TAG, "╠══════════╦══════════╦══════════╦═════════╣");
    ESP_LOGI(TAG, "║  Size    ║  Time(s) ║  Status  ║  Error  ║");
    ESP_LOGI(TAG, "╠══════════╬══════════╬══════════╬═════════╣");

    int pass = 0, fail = 0;
    for (int i = 0; i < (int)NUM_TESTS; i++) {
        ESP_LOGI(TAG, "║  %5d B ║  %5d   ║  %s   ║  %4d   ║",
                 results[i].size,
                 results[i].time_s,
                 results[i].ok ? " OK " : "FAIL",
                 results[i].err_code);
        if (results[i].ok) pass++; else fail++;
    }

    ESP_LOGI(TAG, "╚══════════╩══════════╩══════════╩═════════╝");
    ESP_LOGI(TAG, "  Passed: %d/%d   Failed: %d/%d", pass, (int)NUM_TESTS, fail, (int)NUM_TESTS);
}
#endif /* !TEST_WSS */

/* ── Step 7b: WSS periodic post_log (enabled by #define TEST_WSS) ── */
#ifdef TEST_WSS

/* ── Recovery tuning ─────────────────────────────────────────────── */
#define MAX_SEND_FAILS_BEFORE_RECONNECT  3
#define MAX_RECV_FAILS_BEFORE_RECONNECT  10
#define WS_PING_INTERVAL_SEC             30
#define PDP_HEALTH_CHECK_INTERVAL_SEC    600   /* 10 min */
#define HEAP_LOG_INTERVAL_SEC            60

/* Level 1: WSS reconnect */
#define L1_MAX_ATTEMPTS     5
#define L1_BACKOFF_INIT_MS  5000
#define L1_BACKOFF_MAX_MS   60000

/* Level 2: PDP reactivation */
#define L2_MAX_ATTEMPTS     3

/* Level 3: Modem reboot */
#define L3_MAX_ATTEMPTS     3
#define L3_COOLDOWN_MS      300000   /* 5 min between full reboot cycles */

/* ── Helpers ─────────────────────────────────────────────────────── */

static bool wss_connect(int *conn_ms)
{
    int64_t t0 = esp_timer_get_time();
    gsm_err_t err = gsm_wss_connect(modem, WS_HOST, WS_PORT, WS_PATH, 1, 0);
    *conn_ms = (int)((esp_timer_get_time() - t0) / 1000);

    if (err != GSM_OK) {
        ESP_LOGE(TAG, "  WSS connect FAILED (err=%d, %d ms)", err, *conn_ms);
        return false;
    }
    ESP_LOGI(TAG, "  WSS connected in %d ms", *conn_ms);
    return true;
}

static void log_heap(void)
{
    ESP_LOGI(TAG, "[HEAP] free=%lu  min_ever=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());
}

/** Check PDP context 1 is still active via AT+QIACT? */
static bool pdp_is_active(void)
{
    char resp[256] = {0};
    gsm_send_at_raw(modem, "AT+QIACT?");
    int n = gsm_read_response(modem, resp, sizeof(resp), 2000);
    return (n > 0 && strstr(resp, "+QIACT: 1"));
}

/* ── Level 1: WSS reconnect with exponential backoff ─────────────── */
static bool recovery_level1(void)
{
    int backoff_ms = L1_BACKOFF_INIT_MS;

    for (int attempt = 1; attempt <= L1_MAX_ATTEMPTS; attempt++) {
        ESP_LOGW(TAG, "[L1] WSS reconnect attempt %d/%d", attempt, L1_MAX_ATTEMPTS);

        gsm_ws_close(modem, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));

        int conn_ms;
        if (wss_connect(&conn_ms)) {
            ESP_LOGI(TAG, "[L1] Reconnected on attempt %d", attempt);
            return true;
        }

        ESP_LOGW(TAG, "[L1] Failed — backoff %d ms", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (backoff_ms < L1_BACKOFF_MAX_MS)
            backoff_ms *= 2;
        if (backoff_ms > L1_BACKOFF_MAX_MS)
            backoff_ms = L1_BACKOFF_MAX_MS;
    }

    ESP_LOGE(TAG, "[L1] All %d WSS reconnect attempts failed", L1_MAX_ATTEMPTS);
    return false;
}

/* ── Level 2: PDP reactivation ───────────────────────────────────── */
static bool recovery_level2(void)
{
    for (int attempt = 1; attempt <= L2_MAX_ATTEMPTS; attempt++) {
        ESP_LOGW(TAG, "[L2] PDP reactivation attempt %d/%d", attempt, L2_MAX_ATTEMPTS);

        gsm_ws_close(modem, 0);

        ESP_LOGI(TAG, "[L2] Deactivating PDP context 1...");
        gsm_deactivate_pdp(modem, 1);
        vTaskDelay(pdMS_TO_TICKS(3000));

        ESP_LOGI(TAG, "[L2] Reactivating PDP context 1...");
        if (!step_activate_pdp()) {
            ESP_LOGE(TAG, "[L2] PDP reactivation failed");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));

        int conn_ms;
        if (wss_connect(&conn_ms)) {
            ESP_LOGI(TAG, "[L2] Reconnected after PDP reactivation");
            return true;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    ESP_LOGE(TAG, "[L2] All %d PDP reactivation attempts failed", L2_MAX_ATTEMPTS);
    return false;
}

/* ── Level 3: Full modem reboot ──────────────────────────────────── */
static bool recovery_level3(void)
{
    for (int attempt = 1; attempt <= L3_MAX_ATTEMPTS; attempt++) {
        ESP_LOGE(TAG, "[L3] Modem reboot attempt %d/%d", attempt, L3_MAX_ATTEMPTS);

        gsm_ws_close(modem, 0);

        ESP_LOGI(TAG, "[L3] Rebooting modem (AT+CFUN=1,1)...");
        gsm_reboot(modem);
        /* gsm_reboot already waits 5s for the modem to come back */

        ESP_LOGI(TAG, "[L3] Re-syncing modem...");
        if (!step_begin_modem()) {
            ESP_LOGE(TAG, "[L3] Modem sync failed");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        step_diagnostics();

        ESP_LOGI(TAG, "[L3] Waiting for network...");
        if (!step_wait_network()) {
            ESP_LOGE(TAG, "[L3] Network registration failed");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        ESP_LOGI(TAG, "[L3] Attaching data...");
        if (!step_attach_data()) {
            ESP_LOGE(TAG, "[L3] GPRS attach failed");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        ESP_LOGI(TAG, "[L3] Activating PDP...");
        if (!step_activate_pdp()) {
            ESP_LOGE(TAG, "[L3] PDP activation failed");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(2000));

        int conn_ms;
        if (wss_connect(&conn_ms)) {
            ESP_LOGI(TAG, "[L3] Reconnected after modem reboot");
            return true;
        }

        ESP_LOGE(TAG, "[L3] WSS connect failed after reboot — cooling down %d s",
                 L3_COOLDOWN_MS / 1000);
        vTaskDelay(pdMS_TO_TICKS(L3_COOLDOWN_MS));
    }

    ESP_LOGE(TAG, "[L3] All %d modem reboot attempts failed", L3_MAX_ATTEMPTS);
    return false;
}

/**
 * Escalating recovery: L1 → L2 → L3 → L3 cooldown loop.
 * @param skip_l1  true to skip WSS reconnect (e.g. when PDP is known down).
 * Returns true when WSS is back up.
 */
static bool full_recovery(bool skip_l1)
{
    if (!skip_l1 && recovery_level1()) return true;
    if (recovery_level2()) return true;

    /* L3 loops forever with cooldowns until the modem comes back */
    while (1) {
        if (recovery_level3()) return true;

        ESP_LOGE(TAG, "[RECOVERY] All levels exhausted — cooling down %d s before retrying L3",
                 L3_COOLDOWN_MS / 1000);
        log_heap();
        vTaskDelay(pdMS_TO_TICKS(L3_COOLDOWN_MS));
    }
}

/* ── Main WSS stress loop ────────────────────────────────────────── */

static void step_wss_post_log(void)
{
    ESP_LOGI(TAG, "──── Step 7: WSS Post Log (1 msg/sec) ────");
    ESP_LOGI(TAG, "  Endpoint: wss://%s%s", WS_HOST, WS_PATH);
    log_heap();

    int conn_ms;
    if (!wss_connect(&conn_ms)) {
        ESP_LOGW(TAG, "Initial connect failed — entering recovery");
        if (!full_recovery(false)) return;   /* should not happen (L3 loops) */
    }

    const int size = 8192;

    int seq = 0;
    int last_send_ms = 0;
    int last_recv_ms = 0;
    int last_total_ms = 0;
    int send_fail_streak = 0;
    int recv_fail_streak = 0;

    int64_t last_ping_us   = esp_timer_get_time();
    int64_t last_pdp_chk   = esp_timer_get_time();
    int64_t last_heap_log   = esp_timer_get_time();

    while (1) {
        seq++;
        int64_t now_us = esp_timer_get_time();

        /* ── Periodic heap logging ────────────────────────────── */
        if ((now_us - last_heap_log) / 1000000 >= HEAP_LOG_INTERVAL_SEC) {
            log_heap();
            last_heap_log = now_us;
        }

        /* ── Periodic PDP health check ────────────────────────── */
        if ((now_us - last_pdp_chk) / 1000000 >= PDP_HEALTH_CHECK_INTERVAL_SEC) {
            last_pdp_chk = now_us;
            if (!pdp_is_active()) {
                ESP_LOGE(TAG, "[%d] PDP context lost — skipping L1, entering L2 recovery", seq);
                full_recovery(true);
                send_fail_streak = 0;
                recv_fail_streak = 0;
                last_ping_us = esp_timer_get_time();
                continue;
            }
            ESP_LOGI(TAG, "[%d] PDP health check OK", seq);
        }

        /* ── Periodic WebSocket PING ──────────────────────────── */
        if ((now_us - last_ping_us) / 1000000 >= WS_PING_INTERVAL_SEC) {
            gsm_err_t perr = gsm_ws_ping(modem, 0);
            if (perr != GSM_OK) {
                ESP_LOGW(TAG, "[%d] WS PING failed (err=%d)", seq, perr);
                send_fail_streak++;
            } else {
                ESP_LOGD(TAG, "[%d] WS PING OK", seq);
            }
            last_ping_us = esp_timer_get_time();
        }

        /* ── Check if reconnect needed ────────────────────────── */
        bool need_reconnect = (send_fail_streak >= MAX_SEND_FAILS_BEFORE_RECONNECT) ||
                              (recv_fail_streak >= MAX_RECV_FAILS_BEFORE_RECONNECT);

        if (need_reconnect) {
            ESP_LOGW(TAG, "[%d] Reconnect trigger: send_fails=%d recv_fails=%d",
                     seq, send_fail_streak, recv_fail_streak);
            full_recovery(false);
            send_fail_streak = 0;
            recv_fail_streak = 0;
            last_ping_us = esp_timer_get_time();
            last_pdp_chk = esp_timer_get_time();
            continue;
        }

        /* ── Build payload ────────────────────────────────────── */
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            ESP_LOGW(TAG, "[%d] cJSON alloc failed — retrying", seq);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        cJSON_AddNumberToObject(root, "seq", seq);
        cJSON_AddNumberToObject(root, "size", size);
        cJSON_AddNumberToObject(root, "last_send_ms", last_send_ms);
        cJSON_AddNumberToObject(root, "last_recv_ms", last_recv_ms);
        cJSON_AddNumberToObject(root, "last_total_ms", last_total_ms);

        char *json_no_pad = cJSON_PrintUnformatted(root);
        int meta_len = strlen(json_no_pad);
        cJSON_free(json_no_pad);

        int pad_len = size - meta_len - 10;
        if (pad_len < 1) pad_len = 1;

        char *pad_str = malloc(pad_len + 1);
        if (!pad_str) {
            cJSON_Delete(root);
            ESP_LOGW(TAG, "[%d] pad alloc failed — retrying", seq);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        memset(pad_str, 'A', pad_len);
        pad_str[pad_len] = '\0';

        cJSON_AddStringToObject(root, "data", pad_str);
        free(pad_str);

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!payload) {
            ESP_LOGW(TAG, "[%d] cJSON print failed — retrying", seq);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* ── Send ─────────────────────────────────────────────── */
        int64_t ts = esp_timer_get_time();
        gsm_err_t serr = gsm_ws_send_text(modem, 0, payload, 0);
        last_send_ms = (int)((esp_timer_get_time() - ts) / 1000);
        cJSON_free(payload);

        if (serr != GSM_OK) {
            send_fail_streak++;
            ESP_LOGW(TAG, "[%d] SEND FAIL err=%d (%d B) streak=%d",
                     seq, serr, size, send_fail_streak);
            last_send_ms = -1;
            last_recv_ms = -1;
            last_total_ms = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        send_fail_streak = 0;

        /* ── Receive echo ─────────────────────────────────────── */
        size_t rbuf_size = (size_t)(size + 64);
        char *rbuf = calloc(1, rbuf_size);
        if (!rbuf) {
            ESP_LOGW(TAG, "[%d] rbuf alloc failed — continuing", seq);
            last_recv_ms = -1;
            last_total_ms = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int64_t tr = esp_timer_get_time();
        int n = gsm_ws_recv(modem, 0, rbuf, rbuf_size, 10000);
        last_recv_ms = (int)((esp_timer_get_time() - tr) / 1000);
        last_total_ms = last_send_ms + last_recv_ms;
        free(rbuf);

        if (n == -2) {
            /* Server sent CLOSE frame — immediate reconnect */
            ESP_LOGW(TAG, "[%d] Server sent CLOSE — reconnecting", seq);
            send_fail_streak = MAX_SEND_FAILS_BEFORE_RECONNECT;
            continue;
        } else if (n > 0) {
            recv_fail_streak = 0;
            ESP_LOGI(TAG, "[%d] %5dB send=%dms recv=%dms total=%dms",
                     seq, size, last_send_ms, last_recv_ms, last_total_ms);
        } else {
            recv_fail_streak++;
            ESP_LOGW(TAG, "[%d] RECV FAIL n=%d (%d B) streak=%d send=%dms recv=%dms",
                     seq, n, size, recv_fail_streak, last_send_ms, last_recv_ms);
            last_recv_ms = -1;
            last_total_ms = -1;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif /* TEST_WSS */

/* ── Main ────────────────────────────────────────────────────────── */
void app_main(void)
{
#ifdef TEST_WSS
    ESP_LOGI(TAG, "=== GSM WebSocket Post Log ===\n");
#else
    ESP_LOGI(TAG, "=== GSM HTTPS POST Test ===\n");
#endif

    if (!step_init_modem())   return;
    if (!step_begin_modem())  return;
    step_diagnostics();
    if (!step_wait_network()) return;
    if (!step_attach_data())  return;
    if (!step_activate_pdp()) return;

#ifdef TEST_WSS
    if (!step_upload_ca_cert()) return;
    step_wss_post_log();
#else
    step_stress_test();
#endif

    /* Cleanup */
    gsm_deactivate_pdp(modem, 1);
    gsm_deinit(modem);
    ESP_LOGI(TAG, "Done.");
}

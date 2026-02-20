#include <stdio.h>
#include <string.h>
#include "gsm_modem.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "cJSON.h"

#define TAG "gsm-test"

/* ── Hardware config ─────────────────────────────────────────────── */
#define UART_TX_PIN    18
#define UART_RX_PIN    17
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

static void step_wss_post_log(void)
{
    ESP_LOGI(TAG, "──── Step 7: WSS Post Log (1 msg/sec) ────");
    ESP_LOGI(TAG, "  Endpoint: wss://%s%s", WS_HOST, WS_PATH);

    /* Connect once */
    int64_t t0 = esp_timer_get_time();
    gsm_err_t err = gsm_wss_connect(modem, WS_HOST, WS_PORT, WS_PATH, 1, 0);
    int conn_ms = (int)((esp_timer_get_time() - t0) / 1000);

    if (err != GSM_OK) {
        ESP_LOGE(TAG, "  WSS connect FAILED (err=%d, %d ms)", err, conn_ms);
        return;
    }
    ESP_LOGI(TAG, "  WSS connected in %d ms\n", conn_ms);

    const int size = 8192;

    int seq = 0;
    int last_send_ms = 0;
    int last_recv_ms = 0;
    int last_total_ms = 0;

    while (1) {
        {
            seq++;

            /* Build payload using cJSON */
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

            /* Pad data field to reach target size */
            char *json_no_pad = cJSON_PrintUnformatted(root);
            int meta_len = strlen(json_no_pad);
            cJSON_free(json_no_pad);

            /* overhead: ,"data":"" = 10 chars added by cJSON */
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

            /* Send */
            int64_t ts = esp_timer_get_time();
            gsm_err_t serr = gsm_ws_send_text(modem, 0, payload, 0);
            last_send_ms = (int)((esp_timer_get_time() - ts) / 1000);
            cJSON_free(payload);

            if (serr != GSM_OK) {
                ESP_LOGW(TAG, "[%d] SEND FAIL err=%d (%d B) — continuing", seq, serr, size);
                last_send_ms = -1;
                last_recv_ms = -1;
                last_total_ms = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            /* Receive echo */
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

            if (n > 0) {
                ESP_LOGI(TAG, "[%d] %5dB send=%dms recv=%dms total=%dms",
                         seq, size, last_send_ms, last_recv_ms, last_total_ms);
            } else {
                ESP_LOGW(TAG, "[%d] RECV FAIL n=%d (%d B) send=%dms recv=%dms — continuing",
                         seq, n, size, last_send_ms, last_recv_ms);
                last_recv_ms = -1;
                last_total_ms = -1;
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    gsm_ws_close(modem, 0);
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
    step_wss_post_log();
#else
    step_stress_test();
#endif

    /* Cleanup */
    gsm_deactivate_pdp(modem, 1);
    gsm_deinit(modem);
    ESP_LOGI(TAG, "Done.");
}

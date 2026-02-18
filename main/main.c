#include <stdio.h>
#include <string.h>
#include "gsm_modem.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

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

/* ── HTTPS test endpoint ─────────────────────────────────────────── */
#define HTTPS_URL          "https://rbaskets.in/GSM"
#define HTTPS_URL_FALLBACK "https://httpbin.org/post"
#define HTTPS_BODY         "{\"device\":\"esp32\",\"sensor\":\"temperature\",\"value\":25.5}"

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
    ESP_LOGI(TAG, "──── Step 5: Attach GPRS data (APN: %s) ────", GSM_APN);

    if (gsm_attach_data(modem, GSM_APN, GSM_APN_USER, GSM_APN_PASS, GSM_APN_AUTH)) {
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

    /* Configure context 1 with APN */
    gsm_configure_context(modem, 1, 1, GSM_APN, GSM_APN_USER, GSM_APN_PASS, GSM_APN_AUTH);

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

/* ── Step 7: Perform HTTPS POST ──────────────────────────────────── */
static bool do_https_post(const char *url)
{
    ESP_LOGI(TAG, "  URL:  %s", url);
    ESP_LOGI(TAG, "  Body: %s", HTTPS_BODY);

    char response[2048] = {0};

    int64_t start_us = esp_timer_get_time();

    gsm_err_t err = gsm_https_post(
        modem,
        url,
        HTTPS_BODY,
        response, sizeof(response),
        NULL, 0
    );

    int elapsed_s = (int)((esp_timer_get_time() - start_us) / 1000000);

    if (err == GSM_OK) {
        ESP_LOGI(TAG, "HTTPS POST success! (took %d s)", elapsed_s);
        ESP_LOGI(TAG, "Response:\n%s", response);
        return true;
    }

    ESP_LOGE(TAG, "HTTPS POST failed with error: %d (took %d s)", err, elapsed_s);
    return false;
}

static bool step_https_post(void)
{
    ESP_LOGI(TAG, "──── Step 7: HTTPS POST ────");

    /* Try primary endpoint */
    ESP_LOGI(TAG, "Trying primary: %s", HTTPS_URL);
    if (do_https_post(HTTPS_URL)) {
        return true;
    }

    /* Fallback to httpbin */
    ESP_LOGW(TAG, "Primary failed, trying fallback: %s", HTTPS_URL_FALLBACK);
    vTaskDelay(pdMS_TO_TICKS(2000));
    return do_https_post(HTTPS_URL_FALLBACK);
}

/* ── Main ────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "=== GSM HTTPS POST Test ===\n");

    /* Step 1: Init UART */
    if (!step_init_modem()) return;

    /* Step 2: Sync with modem */
    if (!step_begin_modem()) return;

    /* Step 3: Diagnostics (non-blocking, just info) */
    step_diagnostics();

    /* Step 4: Network registration */
    if (!step_wait_network()) return;

    /* Step 5: GPRS attach */
    if (!step_attach_data()) return;

    /* Step 6: PDP context */
    if (!step_activate_pdp()) return;

    /* Step 7: HTTPS POST */
    if (step_https_post()) {
        ESP_LOGI(TAG, "\n=== ALL STEPS PASSED — HTTPS POST WORKING! ===");
    } else {
        ESP_LOGE(TAG, "\n=== HTTPS POST FAILED ===");
    }

    /* Cleanup */
    gsm_deactivate_pdp(modem, 1);
    gsm_deinit(modem);
    ESP_LOGI(TAG, "Done.");
}

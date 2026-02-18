/*
 * gsm_core.c - Lifecycle, AT engine, utility functions
 * SPDX-License-Identifier: MIT
 */

#include "gsm_private.h"

/* ── UART helpers ─────────────────────────────────────────────────── */

void gsm_flush_input(gsm_handle_t m)
{
    uart_flush_input(m->uart_port);
}

void gsm_uart_write(gsm_handle_t m, const char *data, size_t len)
{
#ifdef GSM_LOG_RAW_UART
    ESP_LOGI(GSM_TAG, "TX [%d]: %.*s", (int)len, (int)len, data);
#endif
    uart_write_bytes(m->uart_port, data, len);
}

void gsm_uart_writeln(gsm_handle_t m, const char *data)
{
#ifdef GSM_LOG_RAW_UART
    ESP_LOGI(GSM_TAG, "TX: %s\\r\\n", data);
#endif
    uart_write_bytes(m->uart_port, data, strlen(data));
    uart_write_bytes(m->uart_port, "\r\n", 2);
}

/* ── Response reading ─────────────────────────────────────────────── */

int gsm_read_response(gsm_handle_t m, char *buf, size_t buf_len, uint32_t timeout_ms)
{
    size_t bytes_read = 0;
    int64_t start = esp_timer_get_time() / 1000;

    while ((esp_timer_get_time() / 1000 - start) < timeout_ms && bytes_read < buf_len - 1) {
        uint8_t c;
        int n = uart_read_bytes(m->uart_port, &c, 1, pdMS_TO_TICKS(10));
        if (n > 0) {
            buf[bytes_read++] = (char)c;
            buf[bytes_read] = '\0';

            /* Early exit on complete response */
            if (strstr(buf, "\r\nOK\r\n") ||
                strstr(buf, "\r\nERROR\r\n") ||
                strstr(buf, "\r\n> ") ||
                strstr(buf, "+CME ERROR:") ||
                strstr(buf, "+QHTTPPOST:") ||
                strstr(buf, "+QHTTPGET:") ||
                strstr(buf, "+QHTTPREAD:")) {
                break;
            }
        }
    }

#ifdef GSM_LOG_RAW_UART
    if (bytes_read > 0) ESP_LOGI(GSM_TAG, "RX [%d]: %s", (int)bytes_read, buf);
#endif
    buf[bytes_read] = '\0';
    return (int)bytes_read;
}

int gsm_collect_response(gsm_handle_t m, char *buf, size_t buf_len, uint32_t timeout_ms)
{
    size_t bytes_read = 0;
    int64_t start = esp_timer_get_time() / 1000;
    bool got_data = false;          /* true once we receive any bytes */
    int64_t last_rx = start;        /* timestamp of last received data */

    while ((esp_timer_get_time() / 1000 - start) < timeout_ms && bytes_read < buf_len - 1) {
        uint8_t tmp[64];
        int n = uart_read_bytes(m->uart_port, tmp, sizeof(tmp), pdMS_TO_TICKS(50));
        if (n > 0) {
            size_t to_copy = (bytes_read + n < buf_len - 1) ? n : (buf_len - 1 - bytes_read);
            memcpy(buf + bytes_read, tmp, to_copy);
            bytes_read += to_copy;
            buf[bytes_read] = '\0';
            got_data = true;
            last_rx = esp_timer_get_time() / 1000;

            /* Standard terminators */
            if (strstr(buf, "\r\nOK\r\n") || strstr(buf, "\r\nERROR\r\n")) {
                break;
            }
            /* HTTP-specific terminators */
            if (strstr(buf, "+QHTTPREAD:") || strstr(buf, "+QHTTPPOST:") || strstr(buf, "+QHTTPGET:")) {
                break;
            }
        } else if (got_data && (esp_timer_get_time() / 1000 - last_rx) > 500) {
            /* No new data for 500ms after receiving something — done */
            break;
        }
    }

#ifdef GSM_LOG_RAW_UART
    if (bytes_read > 0) ESP_LOGI(GSM_TAG, "RX [%d]: %s", (int)bytes_read, buf);
#endif
    buf[bytes_read] = '\0';
    return (int)bytes_read;
}

/* ── AT command engine ────────────────────────────────────────────── */

bool gsm_send_at(gsm_handle_t m, const char *cmd, const char *expect, uint32_t timeout_ms)
{
    if (!expect) expect = "OK";

    ESP_LOGD(GSM_TAG, "CMD: %s", cmd);

    gsm_uart_writeln(m, cmd);

    char buf[GSM_RESP_BUF_SIZE];
    gsm_read_response(m, buf, sizeof(buf), timeout_ms);

    ESP_LOGD(GSM_TAG, "RESP: %s", buf);

    if (strstr(buf, expect)) {
        m->last_error = GSM_OK;
        return true;
    }

    if (strstr(buf, "+CME ERROR:")) {
        m->last_error = (gsm_err_t)gsm_extract_int(buf, "+CME ERROR:");
        return false;
    }

    if (strstr(buf, "+CMS ERROR:")) {
        m->last_error = (gsm_err_t)gsm_extract_int(buf, "+CMS ERROR:");
        return false;
    }

    if (strstr(buf, "ERROR")) {
        m->last_error = GSM_ERR_UNKNOWN;
        return false;
    }

    m->last_error = GSM_ERR_UNKNOWN;
    return false;
}

void gsm_send_at_raw(gsm_handle_t m, const char *cmd)
{
    ESP_LOGD(GSM_TAG, "CMD (raw): %s", cmd);
    gsm_uart_writeln(m, cmd);
}

bool gsm_expect_urc(gsm_handle_t m, const char *tag, uint32_t timeout_ms)
{
    char buf[GSM_RESP_BUF_SIZE];
    gsm_read_response(m, buf, sizeof(buf), timeout_ms);
    return strstr(buf, tag) != NULL;
}

/* ── Utility: extract quoted string ───────────────────────────────── */

bool gsm_extract_quoted(const char *response, const char *tag, char *out, size_t out_len)
{
    const char *p = strstr(response, tag);
    if (!p) return false;

    const char *start = strchr(p, '"');
    if (!start) return false;
    start++;

    const char *end = strchr(start, '"');
    if (!end) return false;

    size_t n = end - start;
    if (n >= out_len) n = out_len - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

/* ── Utility: extract integer after tag ───────────────────────────── */

int gsm_extract_int(const char *response, const char *tag)
{
    const char *p = strstr(response, tag);
    if (!p) return -1;

    p += strlen(tag);
    while (*p && !isdigit((unsigned char)*p) && *p != '-') p++;
    if (*p == '\0') return -1;

    return atoi(p);
}

/* ── Utility: parse CSV integer ───────────────────────────────────── */

int gsm_parse_csv_int(const char *response, const char *tag, int index)
{
    const char *p = strstr(response, tag);
    if (!p) return -1;

    p += strlen(tag);

    for (int i = 0; i < index; i++) {
        p = strchr(p, ',');
        if (!p) return -1;
        p++;
    }

    /* Skip whitespace */
    while (*p == ' ') p++;

    return atoi(p);
}

/* ── Utility: extract first non-empty line ────────────────────────── */

bool gsm_extract_first_line(const char *resp, char *out, size_t out_len)
{
    const char *p = resp;

    /* Skip leading \r\n */
    while (*p == '\r' || *p == '\n') p++;
    if (*p == '\0') return false;

    const char *end = p;
    while (*end && *end != '\r' && *end != '\n') end++;

    size_t n = end - p;
    if (n == 0) return false;
    if (n >= out_len) n = out_len - 1;

    memcpy(out, p, n);
    out[n] = '\0';

    /* Trim trailing spaces */
    while (n > 0 && out[n - 1] == ' ') {
        out[--n] = '\0';
    }

    return true;
}

/* ── Modem initialization sequence ────────────────────────────────── */

static bool initialize_modem(gsm_handle_t m)
{
    ESP_LOGI(GSM_TAG, "Starting AT sync...");

    /* 1. AT sync: send AT every 500ms, up to 10 times */
    bool synced = false;
    for (int i = 0; i < 10; i++) {
        if (gsm_send_at(m, "AT", "OK", 500)) {
            synced = true;
            ESP_LOGI(GSM_TAG, "AT sync OK");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (!synced) {
        ESP_LOGE(GSM_TAG, "AT sync failed");
        m->last_error = GSM_ERR_MODEM_NOT_RESPONDING;
        return false;
    }

    /* 2. Module info */
    gsm_send_at(m, "ATI", "OK", 1000);

    /* 3. Verbose response format */
    gsm_send_at(m, "ATV1", "OK", 1000);

    /* 4. Disable echo */
    if (!m->echo_disabled) {
        if (gsm_send_at(m, "ATE0", "OK", 1000)) {
            m->echo_disabled = true;
            ESP_LOGD(GSM_TAG, "Echo disabled");
        }
    }

    /* 5. Verbose error reporting */
    gsm_send_at(m, "AT+CMEE=2", "OK", 1000);

    /* 6. Query baud rate */
    gsm_send_at(m, "AT+IPR?", "OK", 1000);

    /* 7-10. Identity queries */
    gsm_send_at(m, "AT+GSN", "OK", 1000);
    gsm_send_at(m, "AT+CPIN?", "OK", 1000);
    gsm_send_at(m, "AT+CIMI", "OK", 1000);
    gsm_send_at(m, "AT+QCCID", "OK", 1000);

    /* 11. Signal quality */
    gsm_send_at(m, "AT+CSQ", "OK", 1000);

    /* 12. Network registration */
    gsm_send_at(m, "AT+CREG?", "OK", 1000);
    gsm_send_at(m, "AT+CGREG?", "OK", 1000);
    gsm_send_at(m, "AT+COPS?", "OK", 1000);
    gsm_send_at(m, "AT+CEREG?", "OK", 1000);

    /* Check SIM — can take up to 10s after power-on */
    if (!m->sim_checked) {
        for (int i = 0; i < 10; i++) {
            if (gsm_is_sim_ready(m)) {
                m->sim_checked = true;
                ESP_LOGI(GSM_TAG, "SIM ready");
                break;
            }
            ESP_LOGD(GSM_TAG, "SIM not ready yet (%d/10)...", i + 1);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        if (!m->sim_checked) {
            ESP_LOGE(GSM_TAG, "SIM not ready after 20s");
            m->last_error = GSM_ERR_SIM_NOT_READY;
            return false;
        }
    }

    /* Check signal */
    if (gsm_get_signal_strength(m) < 10) {
        ESP_LOGW(GSM_TAG, "Signal quality low");
        m->last_error = GSM_ERR_SIGNAL_LOW;
        /* Continue — not fatal */
    }

    /* Check GPRS */
    if (!gsm_send_at(m, "AT+CGATT?", "+CGATT: 1", 2000)) {
        ESP_LOGW(GSM_TAG, "GPRS not attached");
        m->last_error = GSM_ERR_GPRS_NOT_ATTACHED;
    }

    /* Update network status */
    int reg = gsm_get_registration_status(m, true);
    m->network_registered = (reg == 1 || reg == 5);
    if (m->network_registered) {
        m->state = GSM_STATE_NETWORK_CONNECTED;
    }

    return true;
}

/* ── Public lifecycle ─────────────────────────────────────────────── */

esp_err_t gsm_init(const gsm_config_t *config, gsm_handle_t *out)
{
    if (!config || !out) return ESP_ERR_INVALID_ARG;

    gsm_handle_t m = calloc(1, sizeof(struct gsm_modem));
    if (!m) return ESP_ERR_NO_MEM;

    m->uart_port  = config->uart_port;
    m->baud_rate  = config->baud_rate;
    m->tx_pin     = config->tx_pin;
    m->rx_pin     = config->rx_pin;
    m->power_pin  = config->power_pin;
    m->state      = GSM_STATE_UNINITIALIZED;
    m->last_error = GSM_OK;

    size_t rx_buf = config->rx_buf_size ? config->rx_buf_size : GSM_DEFAULT_RX_BUF_SIZE;

    uart_config_t uart_cfg = {
        .baud_rate  = config->baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err = uart_driver_install(m->uart_port, rx_buf, 0, 0, NULL, 0);
    if (err != ESP_OK) { free(m); return err; }

    err = uart_param_config(m->uart_port, &uart_cfg);
    if (err != ESP_OK) { uart_driver_delete(m->uart_port); free(m); return err; }

    err = uart_set_pin(m->uart_port, config->tx_pin, config->rx_pin,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) { uart_driver_delete(m->uart_port); free(m); return err; }

    /* Power-on pulse if configured */
    if (config->power_pin >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << config->power_pin,
            .mode         = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        gpio_set_level(config->power_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(config->power_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    *out = m;
    return ESP_OK;
}

gsm_err_t gsm_begin(gsm_handle_t m)
{
    if (!m) return GSM_ERR_INVALID_ARG;

    if (m->initialized) {
        ESP_LOGD(GSM_TAG, "Already initialized");
        return GSM_OK;
    }

    m->state          = GSM_STATE_INITIALIZING;
    m->echo_disabled  = false;
    m->sim_checked    = false;

    vTaskDelay(pdMS_TO_TICKS(1000));
    gsm_flush_input(m);

    if (!initialize_modem(m)) {
        m->state = GSM_STATE_ERROR;
        ESP_LOGE(GSM_TAG, "Modem init failed");
        return m->last_error;
    }

    m->initialized = true;
    m->state = GSM_STATE_READY;
    ESP_LOGI(GSM_TAG, "Modem initialized");
    return GSM_OK;
}

void gsm_deinit(gsm_handle_t m)
{
    if (!m) return;
    uart_driver_delete(m->uart_port);
    free(m);
}

gsm_state_t gsm_get_state(gsm_handle_t m)
{
    return m ? m->state : GSM_STATE_UNINITIALIZED;
}

gsm_err_t gsm_get_last_error(gsm_handle_t m)
{
    return m ? m->last_error : GSM_ERR_INVALID_ARG;
}

/* ── Power management ─────────────────────────────────────────────── */

bool gsm_reboot(gsm_handle_t m)
{
    ESP_LOGI(GSM_TAG, "Rebooting modem...");
    bool ok = gsm_send_at(m, "AT+CFUN=1,1", "OK", 5000);
    if (ok) {
        m->initialized        = false;
        m->echo_disabled      = false;
        m->sim_checked        = false;
        m->network_registered = false;
        m->state              = GSM_STATE_UNINITIALIZED;
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    return ok;
}

bool gsm_power_off(gsm_handle_t m)
{
    ESP_LOGI(GSM_TAG, "Powering off modem...");
    return gsm_send_at(m, "AT+QPOWD=1", "OK", 5000);
}

/* ── SIM / Identity ───────────────────────────────────────────────── */

bool gsm_is_sim_ready(gsm_handle_t m)
{
    return gsm_send_at(m, "AT+CPIN?", "READY", 1000);
}

bool gsm_get_imei(gsm_handle_t m, char *buf, size_t len)
{
    gsm_uart_writeln(m, "AT+GSN");
    char resp[128];
    gsm_read_response(m, resp, sizeof(resp), 1000);

    char line[64];
    if (!gsm_extract_first_line(resp, line, sizeof(line))) return false;

    /* Validate all digits */
    for (int i = 0; line[i]; i++) {
        if (!isdigit((unsigned char)line[i])) return false;
    }

    size_t n = strlen(line);
    if (n >= len) n = len - 1;
    memcpy(buf, line, n);
    buf[n] = '\0';
    return true;
}

int gsm_get_signal_strength(gsm_handle_t m)
{
    gsm_uart_writeln(m, "AT+CSQ");
    char resp[128];
    gsm_read_response(m, resp, sizeof(resp), 1000);
    return gsm_parse_csv_int(resp, "+CSQ: ", 0);
}

bool gsm_get_operator(gsm_handle_t m, char *buf, size_t len)
{
    gsm_uart_writeln(m, "AT+COPS?");
    char resp[256];
    gsm_read_response(m, resp, sizeof(resp), 1000);
    return gsm_extract_quoted(resp, "+COPS:", buf, len);
}

int gsm_get_registration_status(gsm_handle_t m, bool eps)
{
    const char *cmd = eps ? "AT+CEREG?" : "AT+CREG?";
    const char *tag = eps ? "+CEREG: "  : "+CREG: ";

    gsm_uart_writeln(m, cmd);
    char resp[128];
    gsm_read_response(m, resp, sizeof(resp), 1000);
    return gsm_parse_csv_int(resp, tag, 1);
}

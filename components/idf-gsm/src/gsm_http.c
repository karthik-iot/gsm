/*
 * gsm_http.c - HTTP and HTTPS GET/POST via Quectel QHTTP commands
 * SPDX-License-Identifier: MIT
 */

#include "gsm_private.h"

/* ── Internal: send custom headers ────────────────────────────────── */

static void send_http_headers(gsm_handle_t m, const char *headers[], size_t count)
{
    if (!headers || count == 0) return;

    ESP_LOGD(GSM_TAG, "Setting custom HTTP headers");
    if (!gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",1", "OK", 1000)) {
        ESP_LOGW(GSM_TAG, "Failed to enable custom headers");
        return;
    }

    char cmd[256];
    for (size_t i = 0; i < count; i++) {
        if (!headers[i] || !headers[i][0]) continue;
        snprintf(cmd, sizeof(cmd), "AT+QHTTPCFG=\"header\",\"%s\\r\\n\"", headers[i]);
        gsm_send_at(m, cmd, "OK", 1000);
    }
}

/* ── Internal: extract HTTP body from +QHTTPREAD response ─────────── */

static bool extract_http_payload(const char *raw, size_t raw_len, char *out, size_t out_len)
{
    if (!raw_len) return false;
    if (strstr(raw, "ERROR")) return false;

    const char *marker = strstr(raw, "+QHTTPREAD:");
    if (!marker) {
        /* No marker — the whole thing might be the payload */
        size_t n = raw_len < out_len - 1 ? raw_len : out_len - 1;
        memcpy(out, raw, n);
        out[n] = '\0';
        return true;
    }

    /* Skip the +QHTTPREAD: line */
    const char *p = strstr(marker, "\r\n");
    if (!p) {
        size_t n = raw_len < out_len - 1 ? raw_len : out_len - 1;
        memcpy(out, raw, n);
        out[n] = '\0';
        return true;
    }
    p += 2;
    while (*p == '\r' || *p == '\n') p++;

    /* Find trailing OK */
    const char *ok = strstr(p, "\r\nOK");
    size_t payload_len = ok ? (size_t)(ok - p) : strlen(p);

    if (payload_len >= out_len) payload_len = out_len - 1;
    memcpy(out, p, payload_len);
    out[payload_len] = '\0';
    return true;
}

/* ── Core HTTP request engine ─────────────────────────────────────── */

gsm_err_t gsm_http_request(gsm_handle_t m, const char *url, const char *data,
                            char *response, size_t resp_len,
                            const char *headers[], size_t header_count,
                            bool ssl, bool is_post)
{
    /* Flush any leftover data from previous requests */
    gsm_flush_input(m);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Context ID */
    if (!gsm_send_at(m, "AT+QHTTPCFG=\"contextid\",1", "OK", 1000)) {
        m->last_error = GSM_ERR_HTTP_CTX_ID;
        return GSM_ERR_HTTP_CTX_ID;
    }

    /* SSL context if needed */
    if (ssl) {
        if (!gsm_send_at(m, "AT+QHTTPCFG=\"sslctxid\",1", "OK", 1000)) {
            m->last_error = GSM_ERR_HTTP_SSL_CTX_ID;
            return GSM_ERR_HTTP_SSL_CTX_ID;
        }
    }

    /* Custom headers */
    send_http_headers(m, headers, header_count);

    /* Set URL */
    char cmd[64];
    size_t url_len = strlen(url);
    snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%d,10", (int)url_len);

    if (!gsm_send_at(m, cmd, "CONNECT", 5000)) {
        gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
        m->last_error = GSM_ERR_HTTP_URL;
        return GSM_ERR_HTTP_URL;
    }

    /* Send URL in chunks */
    for (size_t i = 0; i < url_len; i += GSM_HTTP_URL_CHUNK_SIZE) {
        size_t chunk = url_len - i;
        if (chunk > GSM_HTTP_URL_CHUNK_SIZE) chunk = GSM_HTTP_URL_CHUNK_SIZE;
        gsm_uart_write(m, url + i, chunk);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (!gsm_expect_urc(m, "OK", 5000)) {
        gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
        m->last_error = GSM_ERR_HTTP_URL_WRITE;
        return GSM_ERR_HTTP_URL_WRITE;
    }

    /* POST or GET */
    if (is_post) {
        size_t data_len = data ? strlen(data) : 0;
        snprintf(cmd, sizeof(cmd), "AT+QHTTPPOST=%d,10,30", (int)data_len);

        if (!gsm_send_at(m, cmd, "CONNECT", 5000)) {
            gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
            m->last_error = GSM_ERR_HTTP_POST;
            return GSM_ERR_HTTP_POST;
        }

        if (data && data_len > 0) {
            gsm_uart_write(m, data, data_len);
        }

        if (!gsm_expect_urc(m, "OK", 5000)) {
            gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
            m->last_error = GSM_ERR_HTTP_POST_DATA;
            return GSM_ERR_HTTP_POST_DATA;
        }

        if (!gsm_expect_urc(m, "+QHTTPPOST:", 15000)) {
            gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
            m->last_error = GSM_ERR_HTTP_POST_URC;
            return GSM_ERR_HTTP_POST_URC;
        }
    } else {
        if (!gsm_send_at(m, "AT+QHTTPGET=60", "OK", 15000)) {
            gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
            m->last_error = GSM_ERR_HTTP_GET;
            return GSM_ERR_HTTP_GET;
        }

        if (!gsm_expect_urc(m, "+QHTTPGET:", 20000)) {
            gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
            m->last_error = GSM_ERR_HTTP_GET_URC;
            return GSM_ERR_HTTP_GET_URC;
        }
    }

    /* Read HTTP response body */
    gsm_send_at_raw(m, "AT+QHTTPREAD");

    char *raw = malloc(GSM_HTTP_RESP_BUF_SIZE);
    if (!raw) {
        gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
        m->last_error = GSM_ERR_HTTP_READ;
        return GSM_ERR_HTTP_READ;
    }

    int n = gsm_collect_response(m, raw, GSM_HTTP_RESP_BUF_SIZE, 10000);

    /* Reset custom headers */
    gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);

    bool ok = extract_http_payload(raw, n, response, resp_len);
    free(raw);

    if (!ok || strstr(response, "ERROR")) {
        m->last_error = GSM_ERR_HTTP_READ;
        return GSM_ERR_HTTP_READ;
    }

    m->last_error = GSM_OK;
    return GSM_OK;
}

/* ── Public HTTP API ──────────────────────────────────────────────── */

gsm_err_t gsm_http_get(gsm_handle_t m, const char *url,
                        char *response, size_t resp_len,
                        const char *headers[], size_t header_count)
{
    return gsm_http_request(m, url, NULL, response, resp_len, headers, header_count, false, false);
}

gsm_err_t gsm_http_post(gsm_handle_t m, const char *url, const char *data,
                         char *response, size_t resp_len,
                         const char *headers[], size_t header_count)
{
    return gsm_http_request(m, url, data, response, resp_len, headers, header_count, false, true);
}

gsm_err_t gsm_https_get(gsm_handle_t m, const char *url,
                         char *response, size_t resp_len,
                         const char *headers[], size_t header_count)
{
    return gsm_http_request(m, url, NULL, response, resp_len, headers, header_count, true, false);
}

gsm_err_t gsm_https_post(gsm_handle_t m, const char *url, const char *data,
                          char *response, size_t resp_len,
                          const char *headers[], size_t header_count)
{
    return gsm_http_request(m, url, data, response, resp_len, headers, header_count, true, true);
}

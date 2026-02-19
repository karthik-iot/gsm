/*
 * gsm_http.c - HTTP and HTTPS GET/POST via Quectel QHTTP commands
 * SPDX-License-Identifier: MIT
 */

#include "gsm_private.h"

/* ── URL parsing helper ──────────────────────────────────────────── */

static void parse_url_parts(const char *url, char *host, size_t host_len,
                            char *path, size_t path_len)
{
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) p += 8;
    else if (strncmp(p, "http://", 7) == 0) p += 7;

    const char *slash = strchr(p, '/');
    if (slash) {
        size_t n = (size_t)(slash - p);
        if (n >= host_len) n = host_len - 1;
        memcpy(host, p, n);
        host[n] = '\0';
        strncpy(path, slash, path_len - 1);
        path[path_len - 1] = '\0';
    } else {
        strncpy(host, p, host_len - 1);
        host[host_len - 1] = '\0';
        path[0] = '/';
        path[1] = '\0';
    }
}

/* ── Calculate raw HTTP header block size ────────────────────────── */

static size_t calc_header_size(const char *method, const char *host,
                               const char *path,
                               const char *headers[], size_t count,
                               size_t body_len)
{
    size_t total = 0;

    /* "POST /path HTTP/1.1\r\n" */
    total += strlen(method) + 1 + strlen(path) + 11;

    /* "Host: <host>\r\n" */
    total += 6 + strlen(host) + 2;

    /* Custom headers, each with \r\n */
    for (size_t i = 0; i < count; i++) {
        if (headers[i] && headers[i][0])
            total += strlen(headers[i]) + 2;
    }

    /* "Content-Length: <N>\r\n" */
    char cl[32];
    total += snprintf(cl, sizeof(cl), "Content-Length: %d\r\n", (int)body_len);

    /* Empty line separating headers from body */
    total += 2;

    return total;
}

/* ── Write raw HTTP headers over UART ────────────────────────────── */

static void write_raw_headers(gsm_handle_t m, const char *method,
                              const char *host, const char *path,
                              const char *headers[], size_t count,
                              size_t body_len)
{
    char line[300];

    snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method, path);
    gsm_uart_write(m, line, strlen(line));

    snprintf(line, sizeof(line), "Host: %s\r\n", host);
    gsm_uart_write(m, line, strlen(line));

    for (size_t i = 0; i < count; i++) {
        if (headers[i] && headers[i][0]) {
            gsm_uart_write(m, headers[i], strlen(headers[i]));
            gsm_uart_write(m, "\r\n", 2);
        }
    }

    snprintf(line, sizeof(line), "Content-Length: %d\r\n", (int)body_len);
    gsm_uart_write(m, line, strlen(line));

    gsm_uart_write(m, "\r\n", 2);
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
    bool has_headers = (headers && header_count > 0);
    char host[128] = {0};
    char path_buf[256] = {0};

    if (has_headers) {
        parse_url_parts(url, host, sizeof(host), path_buf, sizeof(path_buf));
    }

    /* Stop any lingering HTTP session and flush */
    gsm_send_at(m, "AT+QHTTPSTOP", "OK", 1000);
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

    /* Enable raw request header mode if custom headers provided */
    if (has_headers) {
        gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",1", "OK", 1000);
    }

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
        size_t body_len = data ? strlen(data) : 0;
        size_t total_len = body_len;

        if (has_headers) {
            total_len += calc_header_size("POST", host, path_buf,
                                          headers, header_count, body_len);
        }

        snprintf(cmd, sizeof(cmd), "AT+QHTTPPOST=%d,10,60", (int)total_len);

        if (!gsm_send_at(m, cmd, "CONNECT", 5000)) {
            gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
            m->last_error = GSM_ERR_HTTP_POST;
            return GSM_ERR_HTTP_POST;
        }

        /* When requestheader=1, prepend raw HTTP headers before body */
        if (has_headers) {
            write_raw_headers(m, "POST", host, path_buf,
                              headers, header_count, body_len);
        }

        if (data && body_len > 0) {
            gsm_uart_write(m, data, body_len);
        }

        if (!gsm_expect_urc(m, "OK", 5000)) {
            gsm_send_at(m, "AT+QHTTPCFG=\"requestheader\",0", "OK", 1000);
            m->last_error = GSM_ERR_HTTP_POST_DATA;
            return GSM_ERR_HTTP_POST_DATA;
        }

        if (!gsm_expect_urc(m, "+QHTTPPOST:", 60000)) {
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

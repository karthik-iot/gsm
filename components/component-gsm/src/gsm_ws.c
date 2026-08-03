/*
 * gsm_ws.c - WebSocket over TCP/SSL via Quectel AT commands (RFC 6455)
 * SPDX-License-Identifier: MIT
 *
 * Supports both ws:// (plain TCP) and wss:// (SSL/TLS) transports.
 * The modem handles DNS, TCP, and TLS; this file handles the HTTP
 * upgrade handshake and WebSocket frame encode/decode.
 */

#include "gsm_private.h"
#include "esp_random.h"

#define WS_TAG "gsm_ws"

/* ── WebSocket opcodes (RFC 6455 §5.2) ───────────────────────────── */
#define WS_OP_CONT   0x00
#define WS_OP_TEXT   0x01
#define WS_OP_BIN    0x02
#define WS_OP_CLOSE  0x08
#define WS_OP_PING   0x09
#define WS_OP_PONG   0x0A

/* Per-socket SSL flag bitmap (bit N = socket N uses SSL) */
static uint16_t ws_ssl_map = 0;

#define WS_IS_SSL(id)   (ws_ssl_map & (1u << (id)))
#define WS_SET_SSL(id)  (ws_ssl_map |= (1u << (id)))
#define WS_CLR_SSL(id)  (ws_ssl_map &= ~(1u << (id)))

/* ── Timing helper ───────────────────────────────────────────────── */
#ifdef GSM_LOG_WS_TIMING
#define WS_T_START()       int64_t _ws_t0 = esp_timer_get_time(), _ws_tp
#define WS_T_PHASE()       _ws_tp = esp_timer_get_time()
#define WS_T_LOG(label)    ESP_LOGI(WS_TAG, "[WS] %-22s %lld ms", label, \
                                    (esp_timer_get_time() - _ws_tp) / 1000)
#define WS_T_TOTAL()       ESP_LOGI(WS_TAG, "[WS] ─── TOTAL: %lld ms ───", \
                                    (esp_timer_get_time() - _ws_t0) / 1000)
#else
#define WS_T_START()
#define WS_T_PHASE()
#define WS_T_LOG(label)
#define WS_T_TOTAL()
#endif

/* ── Base64 encoder (16 bytes → 24 chars) for Sec-WebSocket-Key ─── */

static void base64_encode_16(const uint8_t in[16], char out[25])
{
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    int j = 0;
    for (int i = 0; i < 15; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) |
                     ((uint32_t)in[i + 1] << 8) |
                     in[i + 2];
        out[j++] = t[(v >> 18) & 0x3F];
        out[j++] = t[(v >> 12) & 0x3F];
        out[j++] = t[(v >> 6)  & 0x3F];
        out[j++] = t[v & 0x3F];
    }
    out[j++] = t[(in[15] >> 2) & 0x3F];
    out[j++] = t[(in[15] << 4) & 0x3F];
    out[j++] = '=';
    out[j++] = '=';
    out[j]   = '\0';
}

/* ── Transport helpers (TCP or SSL, based on per-socket flag) ────── */

static bool ws_transport_send(gsm_handle_t m, int sock_id,
                               const char *data, size_t len)
{
    char cmd[64];
    if (WS_IS_SSL(sock_id))
        snprintf(cmd, sizeof(cmd), "AT+QSSLSEND=%d,%d", sock_id, (int)len);
    else
        snprintf(cmd, sizeof(cmd), "AT+QISEND=%d,%d", sock_id, (int)len);

    if (!gsm_send_at(m, cmd, "> ", 2000)) return false;

    gsm_uart_write(m, data, len);

    char resp[128];
    gsm_read_response(m, resp, sizeof(resp), 5000);
    return strstr(resp, "SEND OK") != NULL;
}

/* AT+QSSLRECV max <read_length> per command (same 1460B MSS limit as send) */
#define WS_AT_RECV_MAX 1460

static int ws_transport_recv(gsm_handle_t m, int sock_id,
                              char *buf, size_t buf_len, uint32_t timeout_ms)
{
    if (WS_IS_SSL(sock_id)) {
        /* Single resp buffer, reused across iterations */
        size_t resp_size = WS_AT_RECV_MAX + 64;
        char *resp = malloc(resp_size);
        if (!resp) return -1;

        int total_read = 0;
        int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
        char cmd[64];

        while ((size_t)total_read < buf_len - 1 && esp_timer_get_time() < deadline) {
            size_t want = buf_len - 1 - total_read;
            if (want > WS_AT_RECV_MAX) want = WS_AT_RECV_MAX;

            snprintf(cmd, sizeof(cmd), "AT+QSSLRECV=%d,%d",
                     sock_id, (int)want);
            gsm_uart_writeln(m, cmd);
            int n = gsm_collect_response(m, resp, resp_size, 2000);
            (void)n;

            const char *tag = "+QSSLRECV: ";
            const char *p = strstr(resp, tag);
            if (!p) { free(resp); return total_read > 0 ? total_read : -1; }

            p += strlen(tag);
            int data_len = atoi(p);

            if (data_len <= 0) {
                if (total_read > 0) break;   /* already have data, return it */
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            const char *data_start = strstr(p, "\r\n");
            if (!data_start) { free(resp); return total_read > 0 ? total_read : -1; }
            data_start += 2;

            int to_copy = data_len;
            if ((size_t)(total_read + to_copy) > buf_len - 1)
                to_copy = buf_len - 1 - total_read;

            memcpy(buf + total_read, data_start, to_copy);
            total_read += to_copy;

            /* Got less than requested → modem buffer drained */
            if (data_len < (int)want) break;
        }

        free(resp);
        if (total_read > 0) buf[total_read] = '\0';
        return total_read;
    }

    return gsm_tcp_recv(m, sock_id, buf, buf_len, timeout_ms);
}

static bool ws_transport_close(gsm_handle_t m, int sock_id)
{
    char cmd[32];
    if (WS_IS_SSL(sock_id)) {
        snprintf(cmd, sizeof(cmd), "AT+QSSLCLOSE=%d", sock_id);
        WS_CLR_SSL(sock_id);
    } else {
        snprintf(cmd, sizeof(cmd), "AT+QICLOSE=%d", sock_id);
    }
    return gsm_send_at(m, cmd, "OK", 5000);
}

/* ── Internal: build and send a single WebSocket frame ───────────── */

/* EC200U AT+QSSLSEND / AT+QISEND max data per command */
#define WS_AT_SEND_MAX 1024

static bool ws_send_frame(gsm_handle_t m, int sock_id, uint8_t opcode,
                          const uint8_t *payload, size_t len)
{
    WS_T_START();

    uint8_t hdr[14];
    int hdr_len = 0;

    hdr[0] = 0x80 | opcode;

    if (len <= 125) {
        hdr[1]  = 0x80 | (uint8_t)len;
        hdr_len = 2;
    } else if (len <= 65535) {
        hdr[1]  = 0x80 | 126;
        hdr[2]  = (len >> 8) & 0xFF;
        hdr[3]  = len & 0xFF;
        hdr_len = 4;
    } else {
        hdr[1] = 0x80 | 127;
        memset(hdr + 2, 0, 4);
        hdr[6] = (len >> 24) & 0xFF;
        hdr[7] = (len >> 16) & 0xFF;
        hdr[8] = (len >> 8)  & 0xFF;
        hdr[9] = len & 0xFF;
        hdr_len = 10;
    }

    uint32_t mask_raw = esp_random();
    uint8_t mask[4];
    memcpy(mask, &mask_raw, 4);
    memcpy(hdr + hdr_len, mask, 4);
    hdr_len += 4;

    size_t total = hdr_len + len;

    if (total <= WS_AT_SEND_MAX) {
        /* ── Small frame: single AT send ─────────────────────────── */
        char cmd[64];
        if (WS_IS_SSL(sock_id))
            snprintf(cmd, sizeof(cmd), "AT+QSSLSEND=%d,%d", sock_id, (int)total);
        else
            snprintf(cmd, sizeof(cmd), "AT+QISEND=%d,%d", sock_id, (int)total);

        WS_T_PHASE();
        if (!gsm_send_at(m, cmd, "> ", 2000)) return false;
        WS_T_LOG("AT prompt:");

        WS_T_PHASE();
        gsm_uart_write(m, (const char *)hdr, hdr_len);

        uint8_t chunk[256];
        for (size_t i = 0; i < len; i += sizeof(chunk)) {
            size_t n = (len - i > sizeof(chunk)) ? sizeof(chunk) : (len - i);
            for (size_t j = 0; j < n; j++)
                chunk[j] = payload[i + j] ^ mask[(i + j) % 4];
            gsm_uart_write(m, (const char *)chunk, n);
        }
        WS_T_LOG("UART write:");

        WS_T_PHASE();
        char resp[128];
        gsm_read_response(m, resp, sizeof(resp), 5000);
        WS_T_LOG("SEND OK wait:");

        WS_T_TOTAL();
        return strstr(resp, "SEND OK") != NULL;
    }

    /* ── Large frame: chunked AT sends ───────────────────────────── */
    ESP_LOGI(WS_TAG, "Chunked send: %d B total (%d B hdr + %d B payload)",
             (int)total, hdr_len, (int)len);

    /* Chunk 1: header + first portion of masked payload */
    WS_T_PHASE();
    size_t first_payload = WS_AT_SEND_MAX - hdr_len;
    if (first_payload > len) first_payload = len;

    uint8_t *buf = malloc(WS_AT_SEND_MAX);
    if (!buf) return false;

    memcpy(buf, hdr, hdr_len);
    for (size_t j = 0; j < first_payload; j++)
        buf[hdr_len + j] = payload[j] ^ mask[j % 4];

    if (!ws_transport_send(m, sock_id, (const char *)buf, hdr_len + first_payload)) {
        free(buf);
        return false;
    }
    WS_T_LOG("Chunk 1 (hdr+data):");

    /* Remaining chunks: masked payload only */
    size_t sent = first_payload;
    int chunk_num = 2;

    while (sent < len) {
        WS_T_PHASE();
        size_t chunk_len = len - sent;
        if (chunk_len > WS_AT_SEND_MAX) chunk_len = WS_AT_SEND_MAX;

        for (size_t j = 0; j < chunk_len; j++)
            buf[j] = payload[sent + j] ^ mask[(sent + j) % 4];

        if (!ws_transport_send(m, sock_id, (const char *)buf, chunk_len)) {
            free(buf);
            return false;
        }

#ifdef GSM_LOG_WS_TIMING
        {
            char label[32];
            snprintf(label, sizeof(label), "Chunk %d:", chunk_num);
            WS_T_LOG(label);
        }
#endif

        sent += chunk_len;
        chunk_num++;
    }

    free(buf);
    WS_T_TOTAL();
    return true;
}

/* ── WebSocket handshake (shared by ws:// and wss://) ────────────── */

static gsm_err_t ws_handshake(gsm_handle_t m, const char *host,
                               const char *path, int sock_id)
{
    WS_T_START();

    uint8_t key_raw[16];
    for (int i = 0; i < 4; i++) {
        uint32_t r = esp_random();
        memcpy(key_raw + i * 4, &r, 4);
    }
    char key_b64[25];
    base64_encode_16(key_raw, key_b64);

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, key_b64);

    ESP_LOGI(WS_TAG, "WS handshake (%d B)", req_len);

    WS_T_PHASE();
    if (!ws_transport_send(m, sock_id, req, req_len)) {
        ESP_LOGE(WS_TAG, "Handshake send failed");
        return GSM_ERR_TCP;
    }
    WS_T_LOG("Handshake send:");

    WS_T_PHASE();
    char resp[512];
    int n = ws_transport_recv(m, sock_id, resp, sizeof(resp), 5000);
    WS_T_LOG("Handshake recv:");

    if (n <= 0) {
        ESP_LOGE(WS_TAG, "No handshake response");
        return GSM_ERR_TIMEOUT;
    }

    ESP_LOGI(WS_TAG, "Handshake resp (%d B): %.128s", n, resp);

    if (!strstr(resp, "101")) {
        ESP_LOGE(WS_TAG, "Upgrade rejected");
        return GSM_ERR_WS_HANDSHAKE;
    }

    WS_T_TOTAL();
    ESP_LOGI(WS_TAG, "WebSocket connected");
    return GSM_OK;
}

/* ── ws:// connect (plain TCP) ───────────────────────────────────── */

gsm_err_t gsm_ws_connect(gsm_handle_t m, const char *host, uint16_t port,
                          const char *path, int ctx_id, int sock_id)
{
    if (!m || !host || !path) return GSM_ERR_INVALID_ARG;

    WS_T_START();
    WS_CLR_SSL(sock_id);

    ESP_LOGI(WS_TAG, "TCP → %s:%d", host, port);

    WS_T_PHASE();
    if (gsm_tcp_open(m, host, port, ctx_id, sock_id) < 0) {
        ESP_LOGE(WS_TAG, "TCP connect failed");
        return GSM_ERR_TCP;
    }
    WS_T_LOG("TCP connect:");

    gsm_err_t err = ws_handshake(m, host, path, sock_id);
    if (err != GSM_OK) ws_transport_close(m, sock_id);

    WS_T_TOTAL();
    return err;
}

/* ── wss:// connect (SSL/TLS) ────────────────────────────────────── */

gsm_err_t gsm_wss_connect(gsm_handle_t m, const char *host, uint16_t port,
                           const char *path, int ctx_id, int sock_id)
{
    if (!m || !host || !path) return GSM_ERR_INVALID_ARG;

    WS_T_START();

    int ssl_ctx = 1;

    /* ── Phase 1: SSL config ─────────────────────────────────────── */
    WS_T_PHASE();

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"sslversion\",%d,4", ssl_ctx);
    gsm_send_at(m, cmd, "OK", 1000);

    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"ciphersuite\",%d,0xFFFF", ssl_ctx);
    gsm_send_at(m, cmd, "OK", 1000);

    /* CA-verified handshake: checks the server cert against the CA
     * already uploaded to the modem as "cacert.pem". */
    gsm_ssl_configure(m, ssl_ctx, "cacert.pem", true);

    snprintf(cmd, sizeof(cmd), "AT+QSSLCFG=\"sni\",%d,1", ssl_ctx);
    gsm_send_at(m, cmd, "OK", 1000);

    WS_T_LOG("Phase 1 SSL config:");

    /* ── Phase 2: SSL socket open (DNS + TCP + TLS handshake) ────── */
    WS_T_PHASE();

    ESP_LOGI(WS_TAG, "SSL → %s:%d", host, port);

    /* Force-close any leftover SSL socket before opening a new one.
     * This prevents "socket already in use" after a prior failed connect. */
    {
        char ccmd[32];
        snprintf(ccmd, sizeof(ccmd), "AT+QSSLCLOSE=%d", sock_id);
        gsm_send_at(m, ccmd, "OK", 2000);          /* ignore error */
    }
    WS_CLR_SSL(sock_id);

    snprintf(cmd, sizeof(cmd),
             "AT+QSSLOPEN=%d,%d,%d,\"%s\",%d,0",
             ctx_id, ssl_ctx, sock_id, host, port);

    if (!gsm_send_at(m, cmd, "OK", 5000)) {
        ESP_LOGE(WS_TAG, "QSSLOPEN cmd failed");
        return GSM_ERR_SSL;
    }

    char expect[32];
    snprintf(expect, sizeof(expect), "+QSSLOPEN: %d,0", sock_id);
    if (!gsm_expect_urc(m, expect, 15000)) {
        ESP_LOGE(WS_TAG, "SSL connect failed");
        /* Close the half-open SSL socket so it can be reused next time */
        char ccmd[32];
        snprintf(ccmd, sizeof(ccmd), "AT+QSSLCLOSE=%d", sock_id);
        gsm_send_at(m, ccmd, "OK", 2000);
        return GSM_ERR_SSL;
    }

    WS_T_LOG("Phase 2 SSL connect:");

    WS_SET_SSL(sock_id);

    /* ── Phase 3+4: WS handshake (send + recv, timed internally) ── */
    gsm_err_t err = ws_handshake(m, host, path, sock_id);
    if (err != GSM_OK) ws_transport_close(m, sock_id);

    WS_T_TOTAL();
    return err;
}

/* ── Send text frame ─────────────────────────────────────────────── */

gsm_err_t gsm_ws_send_text(gsm_handle_t m, int sock_id,
                            const char *msg, size_t len)
{
    if (!m || !msg) return GSM_ERR_INVALID_ARG;
    if (len == 0) len = strlen(msg);

#ifdef GSM_LOG_WS_TIMING
    ESP_LOGI(WS_TAG, "[WS] send_text %d B", (int)len);
#endif

    return ws_send_frame(m, sock_id, WS_OP_TEXT,
                         (const uint8_t *)msg, len) ? GSM_OK : GSM_ERR_TCP;
}

/* ── Send binary frame ───────────────────────────────────────────── */

gsm_err_t gsm_ws_send_binary(gsm_handle_t m, int sock_id,
                              const uint8_t *data, size_t len)
{
    if (!m || !data || len == 0) return GSM_ERR_INVALID_ARG;

#ifdef GSM_LOG_WS_TIMING
    ESP_LOGI(WS_TAG, "[WS] send_binary %d B", (int)len);
#endif

    return ws_send_frame(m, sock_id, WS_OP_BIN,
                         data, len) ? GSM_OK : GSM_ERR_TCP;
}

/* ── Receive one WebSocket frame ─────────────────────────────────── */

int gsm_ws_recv(gsm_handle_t m, int sock_id, char *buf, size_t buf_len,
                uint32_t timeout_ms)
{
    if (!m || !buf || buf_len < 2) return -1;

    WS_T_START();

    /* Heap buffer: payload + up to 14 bytes WS frame header */
    size_t raw_size = buf_len + 16;
    char *raw = malloc(raw_size);
    if (!raw) return -1;

    WS_T_PHASE();
    int raw_len = ws_transport_recv(m, sock_id, raw, raw_size, timeout_ms);
    WS_T_LOG("Transport recv:");

    if (raw_len <= 0) { free(raw); return raw_len; }

    if (raw_len < 2) {
        ESP_LOGW(WS_TAG, "Frame too short (%d B)", raw_len);
        free(raw);
        return -1;
    }

    WS_T_PHASE();

    uint8_t *p = (uint8_t *)raw;
    uint8_t opcode = p[0] & 0x0F;
    bool masked = (p[1] & 0x80) != 0;
    size_t payload_len = p[1] & 0x7F;
    int off = 2;

    if (payload_len == 126) {
        if (raw_len < 4) { free(raw); return -1; }
        payload_len = ((size_t)p[2] << 8) | p[3];
        off = 4;
    } else if (payload_len == 127) {
        if (raw_len < 10) { free(raw); return -1; }
        payload_len = ((size_t)p[6] << 24) | ((size_t)p[7] << 16) |
                      ((size_t)p[8] << 8)  | p[9];
        off = 10;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (raw_len < off + 4) { free(raw); return -1; }
        memcpy(mask, p + off, 4);
        off += 4;
    }

    if (opcode == WS_OP_PING) {
        ESP_LOGD(WS_TAG, "PING → auto PONG");
        ws_send_frame(m, sock_id, WS_OP_PONG, p + off, payload_len);
        free(raw);
        return 0;
    }

    if (opcode == WS_OP_CLOSE) {
        ESP_LOGI(WS_TAG, "Server sent CLOSE");
        ws_send_frame(m, sock_id, WS_OP_CLOSE, p + off,
                      payload_len >= 2 ? 2 : payload_len);
        free(raw);
        return -2;
    }

    size_t avail = (raw_len > off) ? (size_t)(raw_len - off) : 0;
    size_t to_copy = payload_len < avail ? payload_len : avail;
    if (to_copy >= buf_len) to_copy = buf_len - 1;

    memcpy(buf, p + off, to_copy);

    if (masked) {
        for (size_t i = 0; i < to_copy; i++)
            buf[i] ^= mask[i % 4];
    }

    buf[to_copy] = '\0';

    WS_T_LOG("Frame decode:");

#ifdef GSM_LOG_WS_TIMING
    ESP_LOGI(WS_TAG, "[WS] recv: opcode=0x%02X payload=%d B copied=%d B",
             opcode, (int)payload_len, (int)to_copy);
#endif
    WS_T_TOTAL();

    free(raw);
    return (int)to_copy;
}

/* ── Ping ────────────────────────────────────────────────────────── */

gsm_err_t gsm_ws_ping(gsm_handle_t m, int sock_id)
{
    if (!m) return GSM_ERR_INVALID_ARG;
    return ws_send_frame(m, sock_id, WS_OP_PING, NULL, 0)
               ? GSM_OK : GSM_ERR_TCP;
}

/* ── Close (send close frame + tear down TCP/SSL) ────────────────── */

gsm_err_t gsm_ws_close(gsm_handle_t m, int sock_id)
{
    if (!m) return GSM_ERR_INVALID_ARG;

    WS_T_START();

    WS_T_PHASE();
    uint8_t payload[2] = {0x03, 0xE8};     /* 1000 = normal closure */
    ws_send_frame(m, sock_id, WS_OP_CLOSE, payload, 2);
    WS_T_LOG("Close frame:");

    vTaskDelay(pdMS_TO_TICKS(200));

    WS_T_PHASE();
    ws_transport_close(m, sock_id);
    WS_T_LOG("Transport close:");

    WS_T_TOTAL();
    ESP_LOGI(WS_TAG, "WebSocket closed");
    return GSM_OK;
}

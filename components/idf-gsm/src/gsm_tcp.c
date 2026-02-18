/*
 * gsm_tcp.c - TCP socket operations via Quectel QIOPEN/QISEND/QIRD
 * SPDX-License-Identifier: MIT
 */

#include "gsm_private.h"

int gsm_tcp_open(gsm_handle_t m, const char *host, int port, int ctx_id, int socket_id)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+QIOPEN=%d,%d,\"TCP\",\"%s\",%d,0,1",
             ctx_id, socket_id, host, port);

    if (!gsm_send_at(m, cmd, "OK", 5000)) return -1;

    /* Wait for +QIOPEN: <socketId>,0 URC */
    char expect[32];
    snprintf(expect, sizeof(expect), "+QIOPEN: %d,0", socket_id);
    if (!gsm_expect_urc(m, expect, 15000)) return -1;

    return socket_id;
}

bool gsm_tcp_send(gsm_handle_t m, int socket_id, const char *data, size_t len)
{
    if (!data) return false;
    if (len == 0) len = strlen(data);

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+QISEND=%d,%d", socket_id, (int)len);
    if (!gsm_send_at(m, cmd, "> ", 2000)) return false;

    gsm_uart_write(m, data, len);

    char resp[128];
    gsm_read_response(m, resp, sizeof(resp), 5000);
    return strstr(resp, "SEND OK") != NULL;
}

int gsm_tcp_recv(gsm_handle_t m, int socket_id, char *buf, size_t buf_len, uint32_t timeout_ms)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+QIRD=%d,%d", socket_id, (int)(buf_len - 1));

    gsm_uart_writeln(m, cmd);

    char resp[GSM_RESP_BUF_SIZE];
    gsm_read_response(m, resp, sizeof(resp), timeout_ms);

    /* Response: +QIRD: <len>\r\n<data> */
    const char *tag = "+QIRD: ";
    const char *p = strstr(resp, tag);
    if (!p) return -1;

    p += strlen(tag);
    int data_len = atoi(p);
    if (data_len <= 0) return 0;

    /* Find the data start (after \r\n following the length) */
    const char *data_start = strstr(p, "\r\n");
    if (!data_start) return -1;
    data_start += 2;

    int to_copy = data_len;
    if ((size_t)to_copy >= buf_len) to_copy = buf_len - 1;

    memcpy(buf, data_start, to_copy);
    buf[to_copy] = '\0';
    return to_copy;
}

bool gsm_tcp_close(gsm_handle_t m, int socket_id)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+QICLOSE=%d", socket_id);
    return gsm_send_at(m, cmd, "OK", 5000);
}

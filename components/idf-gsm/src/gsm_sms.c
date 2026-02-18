/*
 * gsm_sms.c - SMS send/read/delete
 * SPDX-License-Identifier: MIT
 */

#include "gsm_private.h"

bool gsm_send_sms(gsm_handle_t m, const char *number, const char *text)
{
    /* Text mode */
    if (!gsm_send_at(m, "AT+CMGF=1", "OK", 1000)) return false;

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", number);
    if (!gsm_send_at(m, cmd, ">", 2000)) return false;

    gsm_uart_write(m, text, strlen(text));

    /* Ctrl+Z to send */
    uint8_t ctrlz = 0x1A;
    gsm_uart_write(m, (const char *)&ctrlz, 1);

    char resp[128];
    gsm_read_response(m, resp, sizeof(resp), 10000);
    return strstr(resp, "OK") != NULL;
}

bool gsm_read_sms(gsm_handle_t m, int index, char *buf, size_t len)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);

    gsm_uart_writeln(m, cmd);

    char resp[GSM_RESP_BUF_SIZE];
    gsm_read_response(m, resp, sizeof(resp), 2000);

    /* Response: +CMGR: <stat>,<oa>,<alpha>,<scts>\r\n<data>\r\nOK */
    const char *tag = "+CMGR: ";
    const char *p = strstr(resp, tag);
    if (!p) return false;

    /* Find start of SMS body (after the header line) */
    const char *body_start = strstr(p, "\n");
    if (!body_start) return false;
    body_start++;

    /* Find end before OK */
    const char *body_end = strstr(body_start, "\r\nOK\r\n");
    if (!body_end) body_end = resp + strlen(resp);

    size_t n = body_end - body_start;
    if (n >= len) n = len - 1;
    memcpy(buf, body_start, n);
    buf[n] = '\0';
    return true;
}

bool gsm_delete_sms(gsm_handle_t m, int index)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", index);
    return gsm_send_at(m, cmd, "OK", 1000);
}

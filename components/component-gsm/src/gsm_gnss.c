/*
 * gsm_gnss.c - GNSS/GPS operations
 * SPDX-License-Identifier: MIT
 */

#include "gsm_private.h"

bool gsm_gnss_start(gsm_handle_t m)
{
    return gsm_send_at(m, "AT+QGPS=1", "OK", 1000);
}

bool gsm_gnss_stop(gsm_handle_t m)
{
    return gsm_send_at(m, "AT+QGPSEND", "OK", 1000);
}

bool gsm_gnss_is_on(gsm_handle_t m)
{
    return gsm_send_at(m, "AT+QGPS?", "+QGPS: 1", 1000);
}

bool gsm_gnss_get_location(gsm_handle_t m, char *buf, size_t len)
{
    gsm_uart_writeln(m, "AT+QGPSLOC=2");
    char resp[256];
    gsm_read_response(m, resp, sizeof(resp), 2000);

    const char *tag = "+QGPSLOC: ";
    const char *p = strstr(resp, tag);
    if (!p) return false;

    p += strlen(tag);
    const char *end = strchr(p, '\r');
    if (!end) end = resp + strlen(resp);

    size_t n = end - p;
    if (n >= len) n = len - 1;
    memcpy(buf, p, n);
    buf[n] = '\0';
    return true;
}

bool gsm_gnss_get_nmea(gsm_handle_t m, const char *type, char *buf, size_t len)
{
    if (!type) type = "RMC";

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+QGPSGNMEA=%s", type);

    gsm_uart_writeln(m, cmd);
    char resp[256];
    gsm_read_response(m, resp, sizeof(resp), 1500);

    const char *tag = "+QGPSGNMEA: ";
    const char *p = strstr(resp, tag);
    if (!p) return false;

    p += strlen(tag);
    const char *end = strchr(p, '\r');
    if (!end) end = resp + strlen(resp);

    size_t n = end - p;
    if (n >= len) n = len - 1;
    memcpy(buf, p, n);
    buf[n] = '\0';
    return true;
}

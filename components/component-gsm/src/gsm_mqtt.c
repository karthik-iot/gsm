/*
 * gsm_mqtt.c - MQTT via Quectel QMTOPEN/QMTCONN/QMTPUB/QMTSUB
 * SPDX-License-Identifier: MIT
 */

#include "gsm_private.h"

bool gsm_mqtt_connect(gsm_handle_t m, const char *server, int port, const char *client_id)
{
    if (!client_id) client_id = "ec200u";

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+QMTOPEN=0,\"%s\",%d", server, port);
    if (!gsm_send_at(m, cmd, "+QMTOPEN: 0,0", 15000)) return false;

    snprintf(cmd, sizeof(cmd), "AT+QMTCONN=0,\"%s\"", client_id);
    return gsm_send_at(m, cmd, "+QMTCONN: 0,0", 10000);
}

bool gsm_mqtt_publish(gsm_handle_t m, const char *topic, const char *message)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+QMTPUB=0,0,0,0,\"%s\"", topic);
    if (!gsm_send_at(m, cmd, "> ", 2000)) return false;

    gsm_uart_write(m, message, strlen(message));

    /* Send Ctrl+Z (0x1A) to terminate */
    uint8_t ctrlz = 0x1A;
    gsm_uart_write(m, (const char *)&ctrlz, 1);

    char resp[256];
    gsm_read_response(m, resp, sizeof(resp), 5000);
    return strstr(resp, "OK") != NULL;
}

bool gsm_mqtt_subscribe(gsm_handle_t m, const char *topic)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "AT+QMTSUB=0,1,\"%s\",0", topic);
    return gsm_send_at(m, cmd, "+QMTSUB: 0,1,0", 5000);
}

bool gsm_mqtt_disconnect(gsm_handle_t m)
{
    return gsm_send_at(m, "AT+QMTDISC=0", "OK", 5000);
}

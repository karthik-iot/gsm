/*
 * idf-gsm internal header - not part of the public API
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "gsm_modem.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define GSM_TAG "gsm"

/* Uncomment to log all raw UART TX/RX bytes */
#define GSM_LOG_RAW_UART

#define GSM_DEFAULT_RX_BUF_SIZE  4096
#define GSM_RESP_BUF_SIZE        512
#define GSM_HTTP_RESP_BUF_SIZE   4096
#define GSM_HTTP_URL_CHUNK_SIZE  2048

/* ── Internal modem context ───────────────────────────────────────── */
struct gsm_modem {
    uart_port_t uart_port;
    uint32_t    baud_rate;
    int         tx_pin;
    int         rx_pin;
    int         power_pin;

    gsm_state_t state;
    gsm_err_t   last_error;

    bool initialized;
    bool echo_disabled;
    bool sim_checked;
    bool network_registered;
};

/* ── Internal helpers (shared across source files) ────────────────── */

/** Flush all pending data in the UART RX buffer. */
void gsm_flush_input(gsm_handle_t modem);

/** Send a string (no CRLF appended). */
void gsm_uart_write(gsm_handle_t modem, const char *data, size_t len);

/** Send a string followed by \r\n. */
void gsm_uart_writeln(gsm_handle_t modem, const char *data);

/** Wait for a specific URC substring within timeout. */
bool gsm_expect_urc(gsm_handle_t modem, const char *tag, uint32_t timeout_ms);

/** Collect response until OK/ERROR or timeout. Returns bytes written to buf. */
int gsm_collect_response(gsm_handle_t modem, char *buf, size_t buf_len, uint32_t timeout_ms);

/** Parse a CSV integer at given index from a tagged response line. */
int gsm_parse_csv_int(const char *response, const char *tag, int index);

/** Extract the first non-empty line from a response. */
bool gsm_extract_first_line(const char *resp, char *out, size_t out_len);

/** Internal HTTP request engine (shared by http/https get/post). */
gsm_err_t gsm_http_request(gsm_handle_t modem, const char *url, const char *data,
                            char *response, size_t resp_len,
                            const char *headers[], size_t header_count,
                            bool ssl, bool is_post);

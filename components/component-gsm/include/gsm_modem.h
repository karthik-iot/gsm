/*
 * idf-gsm - ESP-IDF component for Quectel EC200U GSM/LTE modem
 * Ported from the Arduino QuectelEC200U library by misternegative21
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Modem state ──────────────────────────────────────────────────── */
typedef enum {
    GSM_STATE_UNINITIALIZED = 0,
    GSM_STATE_INITIALIZING,
    GSM_STATE_READY,
    GSM_STATE_ERROR,
    GSM_STATE_NETWORK_CONNECTED,
    GSM_STATE_DATA_READY,
} gsm_state_t;

/* ── Error codes ──────────────────────────────────────────────────── */
typedef enum {
    GSM_OK                        =  0,
    GSM_ERR_UNKNOWN               = -1,
    GSM_ERR_MODEM_NOT_RESPONDING  = -2,
    GSM_ERR_SIM_NOT_READY         = -3,
    GSM_ERR_SIGNAL_LOW            = -4,
    GSM_ERR_GPRS_NOT_ATTACHED     = -5,
    GSM_ERR_APN_CONFIG            = -6,
    GSM_ERR_AUTH_CONFIG           = -7,
    GSM_ERR_PDP_ACTIVATION        = -8,
    GSM_ERR_HTTP                  = -10,
    GSM_ERR_HTTP_CTX_ID           = -11,
    GSM_ERR_HTTP_SSL_CTX_ID       = -12,
    GSM_ERR_HTTP_URL              = -13,
    GSM_ERR_HTTP_URL_WRITE        = -14,
    GSM_ERR_HTTP_POST             = -15,
    GSM_ERR_HTTP_POST_DATA        = -16,
    GSM_ERR_HTTP_POST_URC         = -17,
    GSM_ERR_HTTP_GET              = -18,
    GSM_ERR_HTTP_GET_URC          = -19,
    GSM_ERR_HTTP_READ             = -20,
    GSM_ERR_WS                    = -30,
    GSM_ERR_WS_HANDSHAKE          = -31,
    GSM_ERR_MQTT                  = -40,
    GSM_ERR_TCP                   = -50,
    GSM_ERR_SSL                   = -60,
    GSM_ERR_TIMEOUT               = -70,
    GSM_ERR_INVALID_ARG           = -80,
} gsm_err_t;

/* ── Configuration ────────────────────────────────────────────────── */
typedef struct {
    uart_port_t uart_port;      /** UART port number (UART_NUM_1, etc.) */
    int         tx_pin;         /** GPIO for UART TX */
    int         rx_pin;         /** GPIO for UART RX */
    uint32_t    baud_rate;      /** Baud rate, typically 115200 */
    int         power_pin;      /** GPIO to pulse for power-on (-1 to skip) */
    size_t      rx_buf_size;    /** UART RX ring buffer size (0 = default 4096) */
} gsm_config_t;

/** Helper macro for default configuration */
#define GSM_CONFIG_DEFAULT() { \
    .uart_port   = UART_NUM_1, \
    .tx_pin      = 18,         \
    .rx_pin      = 17,         \
    .baud_rate   = 115200,     \
    .power_pin   = -1,         \
    .rx_buf_size = 0,          \
}

/* ── Opaque handle ────────────────────────────────────────────────── */
typedef struct gsm_modem *gsm_handle_t;

/* ── Lifecycle ────────────────────────────────────────────────────── */

/**
 * @brief Create modem instance and configure UART.
 * @param[in]  config  Configuration struct
 * @param[out] out     Handle (set on success)
 * @return ESP_OK on success
 */
esp_err_t gsm_init(const gsm_config_t *config, gsm_handle_t *out);

/**
 * @brief Synchronize with the modem, disable echo, check SIM, etc.
 * @return GSM_OK on success, negative gsm_err_t on failure.
 */
gsm_err_t gsm_begin(gsm_handle_t modem);

/**
 * @brief Destroy modem instance and free resources.
 */
void gsm_deinit(gsm_handle_t modem);

/** Get current modem state. */
gsm_state_t gsm_get_state(gsm_handle_t modem);

/** Get last error code. */
gsm_err_t gsm_get_last_error(gsm_handle_t modem);

/* ── Core AT interface ────────────────────────────────────────────── */

/**
 * @brief Send an AT command and wait for an expected substring in the response.
 * @param cmd     AT command string (e.g. "AT+CSQ")
 * @param expect  Substring to match in response (e.g. "OK"). NULL defaults to "OK".
 * @param timeout_ms  Timeout in milliseconds.
 * @return true if expected substring found.
 */
bool gsm_send_at(gsm_handle_t modem, const char *cmd, const char *expect, uint32_t timeout_ms);

/**
 * @brief Send a raw AT command with no response parsing.
 */
void gsm_send_at_raw(gsm_handle_t modem, const char *cmd);

/**
 * @brief Read response from modem into buffer.
 * @return Number of bytes read.
 */
int gsm_read_response(gsm_handle_t modem, char *buf, size_t buf_len, uint32_t timeout_ms);

/* ── Power management ─────────────────────────────────────────────── */
bool gsm_reboot(gsm_handle_t modem);
bool gsm_power_off(gsm_handle_t modem);

/* ── SIM / Identity ───────────────────────────────────────────────── */
bool gsm_is_sim_ready(gsm_handle_t modem);

/** Write IMEI into buf. Returns true on success. */
bool gsm_get_imei(gsm_handle_t modem, char *buf, size_t len);

/** Write subscriber number (MSISDN) into buf via AT+CNUM. Returns true on success. */
bool gsm_get_sim_number(gsm_handle_t modem, char *buf, size_t len);

/** Signal strength (CSQ value 0-31, or -1 on error). */
int gsm_get_signal_strength(gsm_handle_t modem);

/** Write operator name into buf. */
bool gsm_get_operator(gsm_handle_t modem, char *buf, size_t len);

/**
 * @brief Network registration status.
 * @param eps  true for EPS (LTE), false for CS (2G/3G).
 * @return Registration status integer (1=home, 5=roaming) or -1.
 */
int gsm_get_registration_status(gsm_handle_t modem, bool eps);

/* ── Network / PDP ────────────────────────────────────────────────── */
bool gsm_set_apn(gsm_handle_t modem, const char *apn);

/** Read the currently configured APN from the modem into buf. Returns true on success. */
bool gsm_get_apn(gsm_handle_t modem, char *buf, size_t len);
bool gsm_wait_for_network(gsm_handle_t modem, uint32_t timeout_ms);
bool gsm_attach_data(gsm_handle_t modem, const char *apn, const char *user, const char *pass, int auth);
bool gsm_activate_pdp(gsm_handle_t modem, int ctx_id);
bool gsm_deactivate_pdp(gsm_handle_t modem, int ctx_id);
bool gsm_configure_context(gsm_handle_t modem, int ctx_id, int type, const char *apn,
                           const char *user, const char *pass, int auth);

/* ── HTTP / HTTPS ─────────────────────────────────────────────────── */

/**
 * @brief Perform an HTTP GET request.
 * @param url       Full URL
 * @param response  Buffer to receive body
 * @param resp_len  Size of response buffer
 * @param headers   NULL-terminated array of header strings, or NULL
 * @return GSM_OK on success.
 */
gsm_err_t gsm_http_get(gsm_handle_t modem, const char *url,
                        char *response, size_t resp_len,
                        const char *headers[], size_t header_count);

gsm_err_t gsm_http_post(gsm_handle_t modem, const char *url,
                         const char *data,
                         char *response, size_t resp_len,
                         const char *headers[], size_t header_count);

gsm_err_t gsm_https_get(gsm_handle_t modem, const char *url,
                         char *response, size_t resp_len,
                         const char *headers[], size_t header_count);

gsm_err_t gsm_https_post(gsm_handle_t modem, const char *url,
                          const char *data,
                          char *response, size_t resp_len,
                          const char *headers[], size_t header_count);

/* ── MQTT ─────────────────────────────────────────────────────────── */
bool gsm_mqtt_connect(gsm_handle_t modem, const char *server, int port, const char *client_id);
bool gsm_mqtt_publish(gsm_handle_t modem, const char *topic, const char *message);
bool gsm_mqtt_subscribe(gsm_handle_t modem, const char *topic);
bool gsm_mqtt_disconnect(gsm_handle_t modem);

/* ── WebSocket ────────────────────────────────────────────────────── */

/**
 * @brief Open a TCP connection and perform the WebSocket upgrade handshake.
 * @param host     Server hostname (DNS resolved by modem)
 * @param port     Server port (typically 80 for ws://, 443 for wss://)
 * @param path     Resource path (e.g. "/ws", "/socket.io/?EIO=4")
 * @param ctx_id   PDP context id (usually 1)
 * @param sock_id  Modem socket id (0-11)
 * @return GSM_OK on success, GSM_ERR_WS_HANDSHAKE if server rejects upgrade.
 */
gsm_err_t gsm_ws_connect(gsm_handle_t modem, const char *host, uint16_t port,
                          const char *path, int ctx_id, int sock_id);

/** @brief Same as gsm_ws_connect but over SSL/TLS (wss://). */
gsm_err_t gsm_wss_connect(gsm_handle_t modem, const char *host, uint16_t port,
                           const char *path, int ctx_id, int sock_id);

gsm_err_t gsm_ws_send_text(gsm_handle_t modem, int sock_id,
                            const char *msg, size_t len);

gsm_err_t gsm_ws_send_binary(gsm_handle_t modem, int sock_id,
                              const uint8_t *data, size_t len);

/**
 * @brief Receive one WebSocket message (blocks up to timeout_ms).
 *
 * Automatically responds to PING with PONG.
 * @return >0 payload bytes copied to buf, 0 if control frame only,
 *         -1 on error, -2 if server sent a CLOSE frame.
 */
int gsm_ws_recv(gsm_handle_t modem, int sock_id, char *buf, size_t buf_len,
                uint32_t timeout_ms);

gsm_err_t gsm_ws_ping(gsm_handle_t modem, int sock_id);
gsm_err_t gsm_ws_close(gsm_handle_t modem, int sock_id);

/* ── TCP sockets ──────────────────────────────────────────────────── */

/**
 * @brief Open a TCP connection.
 * @return socket_id on success, -1 on failure.
 */
int gsm_tcp_open(gsm_handle_t modem, const char *host, int port, int ctx_id, int socket_id);
bool gsm_tcp_send(gsm_handle_t modem, int socket_id, const char *data, size_t len);
int  gsm_tcp_recv(gsm_handle_t modem, int socket_id, char *buf, size_t buf_len, uint32_t timeout_ms);
bool gsm_tcp_close(gsm_handle_t modem, int socket_id);

/* ── SMS ──────────────────────────────────────────────────────────── */
bool gsm_send_sms(gsm_handle_t modem, const char *number, const char *text);
bool gsm_read_sms(gsm_handle_t modem, int index, char *buf, size_t len);
bool gsm_delete_sms(gsm_handle_t modem, int index);

/* ── GNSS ─────────────────────────────────────────────────────────── */
bool gsm_gnss_start(gsm_handle_t modem);
bool gsm_gnss_stop(gsm_handle_t modem);
bool gsm_gnss_is_on(gsm_handle_t modem);
bool gsm_gnss_get_location(gsm_handle_t modem, char *buf, size_t len);
bool gsm_gnss_get_nmea(gsm_handle_t modem, const char *type, char *buf, size_t len);

/* ── NTP ──────────────────────────────────────────────────────────── */
bool gsm_ntp_sync(gsm_handle_t modem, const char *server, int timezone, int ctx_id);

/* ── SSL/TLS ──────────────────────────────────────────────────────── */
bool gsm_ssl_configure(gsm_handle_t modem, int ctx_id, const char *ca_path, bool verify);
bool gsm_ssl_upload_cert(gsm_handle_t modem, const char *cert, const char *path);

/* ── DNS ──────────────────────────────────────────────────────────── */
bool gsm_set_dns(gsm_handle_t modem, const char *primary, const char *secondary, int ctx_id);

/* ── Ping ─────────────────────────────────────────────────────────── */
bool gsm_ping(gsm_handle_t modem, const char *host, int ctx_id, int timeout_s, int count);

/* ── Utility ──────────────────────────────────────────────────────── */
bool gsm_extract_quoted(const char *response, const char *tag, char *out, size_t out_len);
int  gsm_extract_int(const char *response, const char *tag);

#ifdef __cplusplus
}
#endif

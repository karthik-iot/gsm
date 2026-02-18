# idf-gsm

ESP-IDF component for Quectel EC200U GSM/LTE modem. Ported from the [Arduino QuectelEC200U](https://github.com/MISTERNEGATIVE21/QuectelEC200U) library.

Tested with **ESP-IDF v4.4.8** on ESP32.

## Adding to your project

Copy or symlink into your project's `components/` directory:

```
your-project/
├── components/
│   └── idf-gsm/    ← this component
├── main/
└── CMakeLists.txt
```

## Quick start

```c
#include "gsm_modem.h"

void app_main(void)
{
    gsm_config_t cfg = GSM_CONFIG_DEFAULT();
    cfg.tx_pin = 17;
    cfg.rx_pin = 16;

    gsm_handle_t modem;
    gsm_init(&cfg, &modem);
    gsm_begin(modem);

    // Connect to network
    gsm_wait_for_network(modem, 60000);
    gsm_attach_data(modem, "your_apn", "", "", 0);
    gsm_activate_pdp(modem, 1);

    // HTTP GET
    char resp[1024];
    gsm_http_get(modem, "http://example.com/api", resp, sizeof(resp), NULL, 0);

    // MQTT
    gsm_mqtt_connect(modem, "broker.example.com", 1883, "device01");
    gsm_mqtt_publish(modem, "sensor/temp", "{\"value\":25.3}");
    gsm_mqtt_disconnect(modem);

    // SMS
    gsm_send_sms(modem, "+1234567890", "Hello from ESP32");

    // GNSS
    gsm_gnss_start(modem);
    char loc[128];
    gsm_gnss_get_location(modem, loc, sizeof(loc));

    gsm_deinit(modem);
}
```

## Configuration

Default pins and baud rate can be changed via `idf.py menuconfig` under **GSM Modem (EC200U)**, or by setting fields on `gsm_config_t` directly:

| Field          | Default       | Description                          |
|----------------|---------------|--------------------------------------|
| `uart_port`    | `UART_NUM_1`  | UART peripheral                      |
| `tx_pin`       | 17            | GPIO for TX                          |
| `rx_pin`       | 16            | GPIO for RX                          |
| `baud_rate`    | 115200        | UART baud rate                       |
| `power_pin`    | -1 (disabled) | GPIO to pulse for modem power-on     |
| `rx_buf_size`  | 4096          | UART RX ring buffer size             |

## API overview

| Category   | Functions                                                            |
|------------|----------------------------------------------------------------------|
| Lifecycle  | `gsm_init`, `gsm_begin`, `gsm_deinit`, `gsm_reboot`, `gsm_power_off` |
| AT engine  | `gsm_send_at`, `gsm_send_at_raw`, `gsm_read_response`               |
| SIM/Info   | `gsm_is_sim_ready`, `gsm_get_imei`, `gsm_get_signal_strength`, `gsm_get_operator` |
| Network    | `gsm_set_apn`, `gsm_wait_for_network`, `gsm_attach_data`, `gsm_activate_pdp` |
| HTTP/S     | `gsm_http_get`, `gsm_http_post`, `gsm_https_get`, `gsm_https_post`  |
| MQTT       | `gsm_mqtt_connect`, `gsm_mqtt_publish`, `gsm_mqtt_subscribe`        |
| TCP        | `gsm_tcp_open`, `gsm_tcp_send`, `gsm_tcp_recv`, `gsm_tcp_close`     |
| SMS        | `gsm_send_sms`, `gsm_read_sms`, `gsm_delete_sms`                   |
| GNSS       | `gsm_gnss_start`, `gsm_gnss_stop`, `gsm_gnss_get_location`         |
| SSL        | `gsm_ssl_configure`, `gsm_ssl_upload_cert`                          |
| NTP/DNS    | `gsm_ntp_sync`, `gsm_set_dns`, `gsm_ping`                          |

See `include/gsm_modem.h` for full documentation.

## License

MIT

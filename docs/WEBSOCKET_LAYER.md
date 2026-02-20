# GSM Modem: WebSocket Layer (ws:// and wss://)

How an ESP32 talks to a Quectel EC200U modem to open a WebSocket connection,
send/receive messages, and handle large payloads — all via AT commands over UART.

Enable/disable timing logs with `GSM_LOG_WS_TIMING` in `gsm_private.h`.

---

## Table of Contents

1. [What is a WebSocket?](#what-is-a-websocket)
2. [How WSS Works Internally (TCP + TLS + Upgrade)](#how-wss-works-internally)
3. [Connection Flow Diagram](#connection-flow-diagram)
4. [The Upgrade Handshake — Simple Explanation](#the-upgrade-handshake)
5. [API Usage](#api-usage)
6. [Usage Without Auth Headers](#usage-without-auth-headers)
7. [Usage With Auth Headers](#usage-with-auth-headers)
8. [Memory Limits & Payload Chunking](#memory-limits--payload-chunking)
9. [AT Command Size Limits per Protocol](#at-command-size-limits-per-protocol)
10. [Send/Receive Chunking — How It Works](#sendreceive-chunking)
11. [Data Flow: Send and Receive](#data-flow-send-and-receive)
12. [Timing Breakdown](#timing-breakdown)
13. [Error Codes](#error-codes)
14. [Changelog (WebSocket-specific)](#changelog)

---

## What is a WebSocket?

A normal HTTP request is like sending a letter — you send one, wait for a reply, done.
A WebSocket is like making a phone call — once connected, both sides can talk
whenever they want, no waiting.

**Why use WebSocket over HTTP?**
- HTTP: one request, one response, connection closed → good for sending sensor data
- WebSocket: persistent connection, bidirectional → good for real-time control,
  live dashboards, push notifications from server

**ws:// vs wss://**
- `ws://` = WebSocket over plain TCP (port 80) — like HTTP
- `wss://` = WebSocket over TLS/SSL (port 443) — like HTTPS, encrypted

---

## How WSS Works Internally

The Quectel EC200U modem does **not** have a built-in WebSocket command. Instead, we
build WebSocket on top of the modem's raw TCP/SSL socket commands. Here's the stack:

```
┌─────────────────────────────────────┐
│  Your Application (ESP32)           │  ← gsm_ws_send_text(), gsm_ws_recv()
├─────────────────────────────────────┤
│  WebSocket Layer (gsm_ws.c)         │  ← frame encode/decode, masking, handshake
├─────────────────────────────────────┤
│  AT+QSSLSEND / AT+QSSLRECV         │  ← raw SSL socket send/recv via AT commands
├─────────────────────────────────────┤
│  Quectel EC200U Modem (firmware)    │  ← handles DNS, TCP, TLS internally
├─────────────────────────────────────┤
│  4G/LTE Radio → Cell Tower → Internet│
└─────────────────────────────────────┘
```

**Key insight:** The modem only knows about TCP and SSL sockets. It has no idea
WebSocket exists. Our code does the WebSocket protocol work:

1. Open an SSL socket to the server (`AT+QSSLOPEN`)
2. Send a special HTTP request asking to "upgrade" to WebSocket
3. If the server agrees ("101 Switching Protocols"), the TCP connection stays open
4. From here on, we send/receive WebSocket **frames** (not raw HTTP)

---

## Connection Flow Diagram

```
  ESP32                          Quectel EC200U                    Server
  ─────                          ──────────────                    ──────

  ╔═══════════════════════════════════════════════════════════════════════╗
  ║  PHASE 1: SSL CONFIGURATION                                         ║
  ╠═══════════════════════════════════════════════════════════════════════╣
  ║                                                                       ║
  ║  AT+QSSLCFG="sslversion",1,4 ──>  "Use TLS 1.2"                     ║
  ║  <────────────────────────────── OK                                   ║
  ║                                                                       ║
  ║  AT+QSSLCFG="ciphersuite",1,0xFFFF ──>  "Accept all ciphers"        ║
  ║  <──────────────────────────────────── OK                             ║
  ║                                                                       ║
  ║  AT+QSSLCFG="seclevel",1,0 ──>  "No certificate verification"       ║
  ║  <──────────────────────────── OK                                     ║
  ║                                                                       ║
  ║  AT+QSSLCFG="sni",1,1 ──>  "Send server name in TLS hello"          ║
  ║  <────────────────────── OK                                           ║
  ║                                                                       ║
  ╠═══════════════════════════════════════════════════════════════════════╣
  ║  PHASE 2: SSL CONNECT (DNS + TCP + TLS handshake)                    ║
  ╠═══════════════════════════════════════════════════════════════════════╣
  ║                                                                       ║
  ║  AT+QSSLOPEN=1,1,0,"wstest.iotready.com",443,0 ──>                  ║
  ║      "Open SSL socket to this host:port"                              ║
  ║  <──────────────────────────────────────────── OK                     ║
  ║                                                                       ║
  ║      (modem internally does: DNS lookup ──────────────> DNS server)   ║
  ║      (modem internally does: TCP connect ─────────────> server:443)   ║
  ║      (modem internally does: TLS handshake ───────────> server)       ║
  ║                                                                       ║
  ║  <──────────────── +QSSLOPEN: 0,0    "SSL connected, socket 0 ready" ║
  ║                                                                       ║
  ╠═══════════════════════════════════════════════════════════════════════╣
  ║  PHASE 3: WEBSOCKET UPGRADE HANDSHAKE                                ║
  ╠═══════════════════════════════════════════════════════════════════════╣
  ║                                                                       ║
  ║  AT+QSSLSEND=0,178 ──>  "I'll send 178 bytes on socket 0"           ║
  ║  <──────────────────── >   (prompt — ready to receive data)           ║
  ║                                                                       ║
  ║  GET /ws HTTP/1.1\r\n             ┐                                   ║
  ║  Host: wstest.iotready.com\r\n    │ HTTP upgrade                      ║
  ║  Upgrade: websocket\r\n           │ request                           ║
  ║  Connection: Upgrade\r\n          │ (sent over              ──────>   ║
  ║  Sec-WebSocket-Key: dGhl...\r\n   │  the SSL socket)                  ║
  ║  Sec-WebSocket-Version: 13\r\n    │                                   ║
  ║  \r\n                              ┘                                   ║
  ║  <──────────────────── SEND OK                                        ║
  ║                                                                       ║
  ║  AT+QSSLRECV=0,512 ──>  "Read up to 512 bytes from socket 0"        ║
  ║  <──────────────────── +QSSLRECV: 129    (129 bytes available)        ║
  ║                                                                       ║
  ║      HTTP/1.1 101 Switching Protocols    ┐                            ║
  ║      Upgrade: websocket                  │ Server agrees!             ║
  ║      Connection: Upgrade                 │ WebSocket is              ║
  ║      Sec-WebSocket-Accept: s3pP...       │ now active                ║
  ║      \r\n                                 ┘                            ║
  ║                                                                       ║
  ║  "101" found → handshake OK! WebSocket is open.                       ║
  ║                                                                       ║
  ╚═══════════════════════════════════════════════════════════════════════╝

  From this point on, the TCP/SSL connection is a WebSocket.
  We send/receive WebSocket FRAMES, not HTTP.

  ╔═══════════════════════════════════════════════════════════════════════╗
  ║  PHASE 4: SEND/RECEIVE MESSAGES                                      ║
  ╠═══════════════════════════════════════════════════════════════════════╣
  ║                                                                       ║
  ║  gsm_ws_send_text(modem, 0, "hello", 5)                              ║
  ║                                                                       ║
  ║      Builds WebSocket frame:                                          ║
  ║      [0x81][0x85][mask×4][masked "hello"]  = 11 bytes                ║
  ║                                                                       ║
  ║  AT+QSSLSEND=0,11 ──>  "Send 11 bytes"                              ║
  ║  <──────────────────── >                                              ║
  ║  (send the 11 frame bytes) ──────────────────────────────> server     ║
  ║  <──────────────────── SEND OK                                        ║
  ║                                                                       ║
  ║  AT+QSSLRECV=0,512 ──>  "Read response"                              ║
  ║  <──────────────────── +QSSLRECV: 7                                   ║
  ║      [0x81][0x05][h][e][l][l][o]    ← server echoes "hello" back     ║
  ║                                                                       ║
  ╠═══════════════════════════════════════════════════════════════════════╣
  ║  PHASE 5: CLOSE                                                       ║
  ╠═══════════════════════════════════════════════════════════════════════╣
  ║                                                                       ║
  ║  gsm_ws_close(modem, 0)                                               ║
  ║      → send Close frame (opcode 0x08, status 1000)                    ║
  ║      → wait 200ms for server to process                               ║
  ║      → AT+QSSLCLOSE=0  "Close the SSL socket"                        ║
  ║  <──────────────────── OK                                             ║
  ║                                                                       ║
  ╚═══════════════════════════════════════════════════════════════════════╝
```

---

## The Upgrade Handshake

The WebSocket connection starts as a normal HTTP request. Here's what happens,
in plain English:

**Step 1 — ESP32 sends an HTTP request:**
```
"Hey server, I want to switch from HTTP to WebSocket.
 Here's a secret key to prove I'm a real WebSocket client."
```
This is the `GET /ws HTTP/1.1` request with `Upgrade: websocket` headers.

**Step 2 — Server responds:**
```
"OK, I accept. Here's my proof I received your key.
 From now on, we speak WebSocket — not HTTP."
```
This is the `101 Switching Protocols` response.

**Step 3 — Both sides switch:**
The TCP connection stays open, but now both sides send/receive **WebSocket frames**
instead of HTTP. The connection stays alive until either side sends a Close frame.

**The Sec-WebSocket-Key:**
- ESP32 generates 16 random bytes → encodes as base64 → sends as `Sec-WebSocket-Key`
- Server hashes the key with a magic GUID → sends back as `Sec-WebSocket-Accept`
- This proves the server actually understands WebSocket (not just any HTTP server)
- Our code generates the key but doesn't verify the Accept hash (not required for operation)

---

## API Usage

### Header File

```c
#include "gsm_modem.h"
```

### Available Functions

| Function | Description |
|----------|-------------|
| `gsm_ws_connect()` | Open `ws://` connection (plain TCP) |
| `gsm_wss_connect()` | Open `wss://` connection (SSL/TLS) |
| `gsm_ws_send_text()` | Send a text message |
| `gsm_ws_send_binary()` | Send binary data |
| `gsm_ws_recv()` | Receive one message (blocks up to timeout) |
| `gsm_ws_ping()` | Send a PING frame |
| `gsm_ws_close()` | Send Close frame and disconnect |

### Function Signatures

```c
/* Connect */
gsm_err_t gsm_ws_connect(gsm_handle_t modem, const char *host, uint16_t port,
                          const char *path, int ctx_id, int sock_id);

gsm_err_t gsm_wss_connect(gsm_handle_t modem, const char *host, uint16_t port,
                           const char *path, int ctx_id, int sock_id);

/* Send */
gsm_err_t gsm_ws_send_text(gsm_handle_t modem, int sock_id,
                            const char *msg, size_t len);   /* len=0 → strlen */

gsm_err_t gsm_ws_send_binary(gsm_handle_t modem, int sock_id,
                              const uint8_t *data, size_t len);

/* Receive */
int gsm_ws_recv(gsm_handle_t modem, int sock_id,
                char *buf, size_t buf_len, uint32_t timeout_ms);
    /* Returns: >0 = payload bytes, 0 = control frame, -1 = error, -2 = server CLOSE */

/* Control */
gsm_err_t gsm_ws_ping(gsm_handle_t modem, int sock_id);
gsm_err_t gsm_ws_close(gsm_handle_t modem, int sock_id);
```

### Parameter Reference

| Parameter | Typical Value | Description |
|-----------|---------------|-------------|
| `modem` | — | Handle from `gsm_init()` |
| `host` | `"example.com"` | Server hostname (DNS resolved by modem) |
| `port` | `80` / `443` | `80` for ws://, `443` for wss:// |
| `path` | `"/ws"` | WebSocket endpoint path |
| `ctx_id` | `1` | PDP context ID (usually 1) |
| `sock_id` | `0` | Modem socket ID (0–11) |
| `timeout_ms` | `10000` | Receive timeout in milliseconds |

---

## Usage Without Auth Headers

Basic echo test — no authentication required:

```c
#include "gsm_modem.h"

/* After modem init, network registration, PDP activation... */

/* 1. Connect */
gsm_err_t err = gsm_wss_connect(modem, "echo.websocket.org", 443, "/", 1, 0);
if (err != GSM_OK) {
    ESP_LOGE(TAG, "WSS connect failed: %d", err);
    return;
}

/* 2. Send a message */
const char *msg = "{\"hello\":\"world\"}";
err = gsm_ws_send_text(modem, 0, msg, 0);   /* 0 = auto strlen */
if (err != GSM_OK) {
    ESP_LOGE(TAG, "Send failed: %d", err);
}

/* 3. Receive the echo */
char buf[512];
int n = gsm_ws_recv(modem, 0, buf, sizeof(buf), 10000);
if (n > 0) {
    ESP_LOGI(TAG, "Received (%d B): %s", n, buf);
} else {
    ESP_LOGE(TAG, "Recv failed: %d", n);
}

/* 4. Close */
gsm_ws_close(modem, 0);
```

---

## Usage With Auth Headers

The current handshake sends standard WebSocket upgrade headers. To add custom
authentication, you have two common approaches:

### Approach 1: Auth via URL query parameters (supported now)

Many WebSocket servers accept tokens in the URL path/query:

```c
/* Token in the path */
gsm_err_t err = gsm_wss_connect(modem,
    "api.example.com", 443,
    "/ws?token=abc123&user=device01",   /* auth in query string */
    1, 0);
```

The server receives the token as part of the HTTP upgrade request:
```
GET /ws?token=abc123&user=device01 HTTP/1.1
Host: api.example.com
Upgrade: websocket
...
```

### Approach 2: Auth via first message after connect

Some servers authenticate via the first WebSocket message:

```c
/* 1. Connect without auth */
gsm_err_t err = gsm_wss_connect(modem, "api.example.com", 443, "/ws", 1, 0);

/* 2. Send auth as first message */
const char *auth = "{\"type\":\"auth\",\"token\":\"abc123\"}";
gsm_ws_send_text(modem, 0, auth, 0);

/* 3. Receive auth confirmation */
char buf[256];
int n = gsm_ws_recv(modem, 0, buf, sizeof(buf), 5000);
/* Check if server accepted the auth... */
```

### Approach 3: Custom HTTP headers (requires code change)

If your server requires `Authorization:` or custom headers in the upgrade request,
you would modify the `ws_handshake()` function in `gsm_ws.c`:

```c
/* In ws_handshake(), add headers to the request: */
int req_len = snprintf(req, sizeof(req),
    "GET %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Upgrade: websocket\r\n"
    "Connection: Upgrade\r\n"
    "Sec-WebSocket-Key: %s\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Authorization: Bearer %s\r\n"       /* ← add your header here */
    "\r\n",
    path, host, key_b64, auth_token);
```

> **Note:** The `req` buffer is 512 bytes. If your headers are long, increase this.

---

## Memory Limits & Payload Chunking

### The Problem

The Quectel EC200U has **hard limits** on how much data you can send or receive
in a single AT command:

```
AT+QSSLSEND=0,2056    → ERROR    ← too large, modem rejects it
AT+QSSLSEND=0,1024    → >        ← OK, within limit
```

The limit is approximately **1460 bytes** per AT command (matches TCP MSS — Maximum
Segment Size). This is the raw TCP packet size the modem can handle in one shot.

### The Solution: Chunking

The WebSocket layer automatically chunks large payloads. You don't need to do
anything special — just call `gsm_ws_send_text()` with any size:

```c
/* This "just works" even though it's 16KB */
char *big_payload = malloc(16384);
/* ... fill it ... */
gsm_ws_send_text(modem, 0, big_payload, 16384);
```

Internally, the code splits it into chunks:

```
16384 B payload + 8 B header + 4 B mask = 16396 B total

Chunk 1: AT+QSSLSEND=0,1024   → [WS header + first 1016 B of masked payload]
Chunk 2: AT+QSSLSEND=0,1024   → [next 1024 B of masked payload]
Chunk 3: AT+QSSLSEND=0,1024   → [next 1024 B]
  ...
Chunk 17: AT+QSSLSEND=0,252   → [last 252 B of masked payload]
```

### Memory Usage

For a given payload size, here's how much heap memory is used simultaneously:

| Payload | Send heap | Recv heap | Total peak | Notes |
|---------|-----------|-----------|------------|-------|
| 100 B | 1 KB | ~1.6 KB | ~2.6 KB | Single-shot send, small recv buffer |
| 1 KB | 1 KB | ~2.6 KB | ~3.6 KB | Single-shot send |
| 4 KB | 1 KB | ~9.7 KB | ~10.7 KB | Chunked send, chunked recv |
| 8 KB | 1 KB | ~17.6 KB | ~18.6 KB | Chunked send, chunked recv |
| 16 KB | 1 KB | ~34 KB | ~35 KB | Chunked send, chunked recv |

**Send** always uses ~1 KB (one chunk buffer, reused for each chunk).

**Receive** is where the memory goes:
- `raw` buffer: `payload + 16` bytes (to hold incoming WS frame with header)
- `resp` buffer: `1460 + 64` bytes (for AT command response parsing)
- Your application's `buf`: whatever size you allocate

**ESP32 free heap:** Typically ~200 KB. A 16 KB payload using ~35 KB is well within limits.

### Constants

```c
#define WS_AT_SEND_MAX  1024    /* Max bytes per AT+QSSLSEND command */
#define WS_AT_RECV_MAX  1460    /* Max bytes per AT+QSSLRECV command */
```

---

## AT Command Size Limits per Protocol

Different Quectel AT command sets have different size limits:

| Protocol | AT Command | Max per command | Who handles chunking? |
|----------|------------|-----------------|----------------------|
| **HTTP/HTTPS** | `AT+QHTTPPOST=<total>` | **No limit** (modem handles it) | Modem firmware |
| **TCP socket** | `AT+QISEND=<id>,<len>` | **~1460 B** (TCP MSS) | Your code |
| **SSL socket** | `AT+QSSLSEND=<id>,<len>` | **~1460 B** (TCP MSS) | Your code |
| **SSL recv** | `AT+QSSLRECV=<id>,<len>` | **~1460 B** (TCP MSS) | Your code |
| **MQTT** | `AT+QMTPUBEX` | **Configurable** (up to 4 KB default) | Modem firmware |

**Key difference:** The HTTP engine (`AT+QHTTPPOST`) has its own internal buffering,
so you can POST 16 KB in one call. Raw TCP/SSL sockets expose the transport directly —
you must chunk at the TCP MSS boundary.

---

## Send/Receive Chunking

### How Send Chunking Works

```
┌─────────────────────────────────────────────────────────────┐
│  gsm_ws_send_text(modem, 0, payload, 4096)                  │
│                                                               │
│  1. Build WebSocket frame header (6-14 bytes)                │
│     [FIN+opcode] [MASK+length] [mask_key×4]                 │
│                                                               │
│  2. Total = header + payload = 4102 bytes                    │
│     4102 > WS_AT_SEND_MAX (1024) → use chunked path         │
│                                                               │
│  3. Allocate 1 KB buffer                                     │
│                                                               │
│  4. Chunk 1: [header 6B] + [first 1018B masked payload]     │
│     → AT+QSSLSEND=0,1024 → SEND OK                         │
│                                                               │
│  5. Chunk 2: [next 1024B masked payload]                     │
│     → AT+QSSLSEND=0,1024 → SEND OK                         │
│                                                               │
│  6. Chunk 3: [next 1024B masked payload]                     │
│     → AT+QSSLSEND=0,1024 → SEND OK                         │
│                                                               │
│  7. Chunk 4: [last 1030B masked payload]                     │
│     → AT+QSSLSEND=0,1030 → SEND OK                         │
│                                                               │
│  8. Free buffer, return GSM_OK                               │
└─────────────────────────────────────────────────────────────┘
```

**Important:** The masking happens *before* sending. Per RFC 6455, clients must
XOR every payload byte with a random 4-byte mask. This is why TX UART logs show
binary/garbage — it's masked data, not a bug.

### How Receive Chunking Works

```
┌─────────────────────────────────────────────────────────────┐
│  gsm_ws_recv(modem, 0, buf, 4160, 10000)                    │
│                                                               │
│  1. Allocate raw buffer (4160 + 16 = 4176 bytes)            │
│  2. Call ws_transport_recv() to fill the raw buffer          │
│                                                               │
│  ws_transport_recv() internally:                             │
│  ┌───────────────────────────────────────────────────────┐   │
│  │ total_read = 0                                         │   │
│  │                                                         │   │
│  │ Loop iteration 1:                                       │   │
│  │   want = min(4175, 1460) = 1460                        │   │
│  │   AT+QSSLRECV=0,1460 → +QSSLRECV: 1460               │   │
│  │   memcpy into raw[0..1459]                             │   │
│  │   total_read = 1460                                     │   │
│  │                                                         │   │
│  │ Loop iteration 2:                                       │   │
│  │   want = min(2715, 1460) = 1460                        │   │
│  │   AT+QSSLRECV=0,1460 → +QSSLRECV: 1460               │   │
│  │   memcpy into raw[1460..2919]                          │   │
│  │   total_read = 2920                                     │   │
│  │                                                         │   │
│  │ Loop iteration 3:                                       │   │
│  │   want = min(1255, 1460) = 1255                        │   │
│  │   AT+QSSLRECV=0,1255 → +QSSLRECV: 1182               │   │
│  │   memcpy into raw[2920..4101]                          │   │
│  │   total_read = 4102                                     │   │
│  │   1182 < 1255 → modem buffer drained → break           │   │
│  └───────────────────────────────────────────────────────┘   │
│                                                               │
│  3. Parse WebSocket frame header from raw buffer             │
│  4. Unmask payload if masked (server → client is unmasked)   │
│  5. Copy payload into user's buf                             │
│  6. Free raw buffer, return payload length                   │
└─────────────────────────────────────────────────────────────┘
```

**Polling for first data:** `AT+QSSLRECV` is **non-blocking**. If no data has
arrived yet, it returns `+QSSLRECV: 0` immediately. The recv loop polls every
100ms until data arrives or the timeout expires.

---

## Data Flow: Send and Receive

### Send Flow (text message)

```
                         gsm_ws_send_text()
                               │
                    ┌──────────┴──────────┐
                    │ Build WS frame:     │
                    │  [FIN=1, opcode=1]  │
                    │  [MASK=1, length]   │
                    │  [4-byte mask key]  │
                    │  [XOR payload]      │
                    └──────────┬──────────┘
                               │
                    total ≤ 1024?
                   ┌─── yes ───┴─── no ───┐
                   │                       │
          Single AT+QSSLSEND     Loop: chunked sends
          (header + masked data)  (1024 B each, last may
                   │               be smaller)
                   │                       │
                   └───────────┬───────────┘
                               │
                         SEND OK
```

### Receive Flow

```
                         gsm_ws_recv()
                               │
                    ┌──────────┴──────────┐
                    │ Allocate raw buffer  │
                    │ (buf_len + 16)       │
                    └──────────┬──────────┘
                               │
                    ws_transport_recv()
                    (poll loop with 1460B reads)
                               │
                    ┌──────────┴──────────┐
                    │ Parse WS frame hdr  │
                    │ Check opcode:       │
                    └──────────┬──────────┘
                               │
              ┌────────┬───────┴────────┬──────────┐
              │        │                │          │
           TEXT/BIN   PING            CLOSE      Error
           (data)     auto PONG       echo CLOSE  return -1
           copy to    return 0        return -2
           user buf
              │
         return bytes copied
```

---

## Timing Breakdown

### Typical WSS Connection + Send/Recv Timing

```
0ms                                                              ~12000ms
 ├──────────┬──────────────────┬──────────┬──────────┬──────────┤
 │ SSL cfg  │  SSL connect     │ Handshake│  Send    │ Recv     │
 │ ~50ms    │  ~7000-9000ms    │ ~1500ms  │ varies   │ varies   │
 │          │  (DNS+TCP+TLS)   │ (send+   │          │          │
 │ local    │  NETWORK         │  recv)   │          │          │
 └──────────┴──────────────────┴──────────┴──────────┴──────────┘
```

**First connection** is slow (~10-12 seconds) because the modem must:
1. DNS resolve the hostname
2. TCP three-way handshake
3. Full TLS handshake (key exchange, certificates)

**Subsequent messages** on the same connection are fast:
- Send 100B: ~150-300 ms
- Send 1KB: ~300-500 ms
- Send 4KB: ~800-1500 ms (chunked)
- Send 16KB: ~3000-5000 ms (chunked)

**Receive** depends on:
- Server response time
- Payload size (multiple `AT+QSSLRECV` calls for large responses)
- 100ms polling interval if data hasn't arrived yet

---

### What Send Time (send_ms) Measures

`send_ms` is measured in `main.c` around the `gsm_ws_send_text()` call. It covers
the entire path from ESP32 to the cellular network:

```
                          gsm_ws_send_text()
                                │
                         ws_send_frame()
                                │
               ┌────────────────┼────────────────┐
               │                                  │
          total ≤ 1024 B                    total > 1024 B
          (single shot)                     (chunked path)
               │                                  │
   ┌───────────┴───────────┐         ┌────────────┴────────────┐
   │ 1. AT+QSSLSEND=0,N   │         │ For each 1024 B chunk:  │
   │    wait for ">" prompt│         │  1. AT+QSSLSEND=0,1024 │
   │ 2. UART write:        │         │     wait for ">" prompt │
   │    header + masked    │         │  2. UART write: masked  │
   │    payload            │         │     payload chunk       │
   │ 3. Wait for "SEND OK" │         │  3. Wait for "SEND OK" │
   │    (modem encrypts    │         │     (repeat for each    │
   │    + TCP transmit)    │         │      chunk)             │
   └───────────────────────┘         └─────────────────────────┘
```

**What's included in send_ms:**
1. AT command prompt wait (~5-10 ms per chunk)
2. UART serialization at 115200 baud (~0.087 ms/byte)
3. Modem TLS encryption
4. Modem TCP transmit to cellular network
5. "SEND OK" response wait

### Why Send Time Scales Linearly

The bottleneck is the **UART at 115200 baud** (~11.5 KB/s). The data path is:

```
ESP32 → UART (115200) → Modem AT+QSSLSEND → TLS encrypt → cellular TX
```

Observed rate: **~0.1 ms per byte** (matching the 115200 baud theoretical limit):

| Payload | Send(ms) | Rate (ms/B) |
|---------|----------|-------------|
| 100 B   | 16       | 0.16        |
| 256 B   | 30       | 0.12        |
| 512 B   | 52       | 0.10        |
| 1024 B  | 111      | 0.11        |
| 2048 B  | 212      | 0.10        |
| 4096 B  | 413      | 0.10        |
| 8192 B  | 815      | 0.10        |
| 16384 B | 1622     | 0.10        |

**Formula:** `send_ms ≈ 16 + (payload_bytes × 0.098)`

For payloads >1024 B, the chunked send path adds per-chunk AT command overhead
(~5-10 ms per `AT+QSSLSEND` round-trip), but this is small compared to UART time.

---

### What Recv Time (recv_ms) Measures

`recv_ms` is measured in `main.c` around the `gsm_ws_recv()` call. It covers
waiting for the server response + reading it from the modem:

```
                          gsm_ws_recv()
                                │
                    ┌───────────┴───────────┐
                    │ Allocate raw buffer    │
                    │ (payload + 16 bytes)   │
                    └───────────┬───────────┘
                                │
                     ws_transport_recv()
                    ┌───────────┴───────────┐
                    │ Poll loop:            │
                    │                       │
                    │  AT+QSSLRECV=0,1460   │◄──┐
                    │         │              │   │
                    │    +QSSLRECV: N        │   │
                    │         │              │   │
                    │    N > 0?              │   │
                    │   yes/    \no          │   │
                    │   │        │           │   │
                    │ copy     sleep 100ms ──┘   │
                    │ data       (retry)         │
                    │   │                        │
                    │ N < want?                  │
                    │   yes → buffer drained     │
                    │   no  → read more          │
                    └───────────┬───────────┘
                                │
                    ┌───────────┴───────────┐
                    │ Decode WS frame       │
                    │ (parse header, unmask) │
                    └───────────────────────┘
```

**What's included in recv_ms:**
1. Network round-trip — waiting for server to receive, process, and reply
2. Cellular uplink/downlink latency (50-300 ms, highly variable on 4G)
3. Modem TLS decryption
4. AT+QSSLRECV polling — each empty poll adds **100 ms** of dead time
5. UART read (modem → ESP32)
6. WebSocket frame header parsing and unmasking

### Why Recv Time Is Unpredictable

Recv time is governed by **4 stacked sources of jitter**:

```
recv_ms = network_rtt + server_processing + poll_overhead + uart_read

         ┌─────────────────────────────────────────────────┐
         │             Sources of recv jitter               │
         ├──────────────────────┬──────────────────────────┤
         │ 1. Cellular latency  │ 50-300ms per packet,     │
         │    (dominant)        │ can spike to 500ms+      │
         │                      │ depending on signal,     │
         │                      │ congestion, handover     │
         ├──────────────────────┼──────────────────────────┤
         │ 2. Server echo time  │ Varies with server load  │
         │                      │ and response payload     │
         ├──────────────────────┼──────────────────────────┤
         │ 3. QSSLRECV polling  │ Each empty poll = +100ms │
         │    (quantized)       │ of dead time. 3 empty    │
         │                      │ polls = 300ms wasted     │
         ├──────────────────────┼──────────────────────────┤
         │ 4. Modem buffer      │ TLS record may arrive    │
         │    fragmentation     │ in pieces; partial data  │
         │                      │ → poll again → +100ms    │
         └──────────────────────┴──────────────────────────┘
```

### Explaining the 4096 B Anomaly (55 ms recv)

```
╠══════════╬══════════╬══════════╣
║   2048 B ║    212   ║    977   ║  send short  → recv waits for network
║   4096 B ║    413   ║     55   ║  send long   → response already buffered!
║   8192 B ║    815   ║   1894   ║  send longer → but response is also larger
╠══════════╬══════════╬══════════╣
```

The 4096 B case has the **lowest recv time** because of a timing overlap:

1. Sending 4096 B took **413 ms** (long enough for the server to start replying)
2. While the ESP32 was still writing chunks to UART, the server had already:
   - Received the full payload
   - Processed it
   - Sent the echo response back
   - The modem had buffered the response in its internal buffer
3. When `gsm_ws_recv()` was called, the data was **already waiting** in the modem
4. First `AT+QSSLRECV` returned data immediately → no 100 ms poll delays

Compare with 100 B (recv = 170 ms): send finished in only 16 ms — far too fast for
the server to have replied. Recv had to poll and wait for the network round-trip.

For 8192 B (recv = 1894 ms): although send took 815 ms, the server's echo response
is also 8 KB, which takes longer to travel back over the cellular link and requires
multiple `AT+QSSLRECV` calls with possible empty polls between them.

### End-to-End Timing Model

For a WebSocket echo (send N bytes, receive N bytes back), the total time is:

```
total_ms ≈ send_time + max(0, network_rtt - send_time) + recv_transfer_time

Where:
  send_time          ≈ 16 + (N × 0.098) ms       (UART-limited, predictable)
  network_rtt        ≈ 200-1000 ms                (cellular, unpredictable)
  recv_transfer_time ≈ (N / 1460) × AT_overhead   (chunked reads from modem)
  AT_overhead        ≈ 15-30 ms per QSSLRECV call
```

**Key insight:** When `send_time > network_rtt`, the recv appears near-instant
(the 4096 B case). When `send_time < network_rtt`, recv includes the wait.

---

## Error Codes

| Code | Name | Meaning |
|------|------|---------|
| `0` | `GSM_OK` | Success |
| `-30` | `GSM_ERR_WS` | General WebSocket error |
| `-31` | `GSM_ERR_WS_HANDSHAKE` | Server rejected the upgrade (no "101" in response) |
| `-50` | `GSM_ERR_TCP` | TCP/socket send or receive failed |
| `-60` | `GSM_ERR_SSL` | SSL open or config failed |
| `-70` | `GSM_ERR_TIMEOUT` | No handshake response within timeout |
| `-80` | `GSM_ERR_INVALID_ARG` | NULL pointer or invalid parameter |

`gsm_ws_recv()` return values:
| Return | Meaning |
|--------|---------|
| `> 0` | Number of payload bytes copied to buffer |
| `0` | Control frame handled (e.g., PING → auto PONG) |
| `-1` | Error (timeout, parse failure, transport error) |
| `-2` | Server sent CLOSE frame (connection ending) |

---

## Changelog

### WebSocket Layer Changes (this session)

**1. Fixed WSS handshake failure**
- **Problem:** `AT+QSSLRECV` is non-blocking/poll-based — returns `+QSSLRECV: 0`
  immediately if no data has arrived yet. First call after handshake send always
  returned 0, causing "No handshake response" error.
- **Fix:** `ws_transport_recv()` now polls in a retry loop (100ms intervals)
  until data arrives or timeout expires.

**2. Added send chunking for large payloads**
- **Problem:** `AT+QSSLSEND=0,2056` → ERROR. The modem rejects sends > ~1460B.
- **Fix:** `ws_send_frame()` splits frames larger than `WS_AT_SEND_MAX` (1024B)
  into multiple `AT+QSSLSEND` calls. Chunk 1 includes the WS header + first
  portion of masked payload. Remaining chunks are masked payload only.

**3. Added receive chunking for large payloads**
- **Problem:** `AT+QSSLRECV=0,4096` → ERROR. Same 1460B limit applies to recv.
- **Fix:** `ws_transport_recv()` reads in a loop, requesting at most
  `WS_AT_RECV_MAX` (1460B) per `AT+QSSLRECV` call, accumulating into the buffer
  until the modem's buffer is drained or the user's buffer is full.

**4. Fixed binary data in UART TX logs**
- **Problem:** `gsm_uart_write()` used `%.*s` format which printed masked WebSocket
  bytes as garbage characters in the serial monitor.
- **Fix:** `gsm_uart_write()` now detects binary data (checks first 16 bytes for
  non-printable characters) and logs a hex dump (first 32 bytes) instead.

**5. Fixed test code recv buffer too small**
- **Problem:** `char rbuf[512]` truncated responses for payloads > 512B.
- **Fix:** Heap-allocated recv buffer: `calloc(1, max(512, payload_size + 64))`.

**6. Fixed test results marking recv failures as OK**
- **Problem:** `ws_results[i].ok = true` was set regardless of recv result.
- **Fix:** `.ok = true` only when `n > 0` (actual data received).

**7. Added total time column to WSS test results**
- Added `total_ms` field to `ws_result_t` and `Total(s)` column in summary table.

---

## Test Results

Real test output from ESP32 + Quectel EC200U over Airtel 4G/LTE (CSQ 31/31),
WSS echo server at `wss://wstest.iotready.com/ws`.

### Connection

```
SSL → wstest.iotready.com:443
WSS connected in 15429 ms    (DNS + TCP + TLS + WS handshake, first connect)
```

### Payload Tests (Run 1)

```
╔══════════════════════════════════════════════════════════════════╗
║               WSS Stress Test Results                           ║
╠══════════╦══════════╦══════════╦══════════╦═════════╦════════════╣
║  Size    ║ Send(ms) ║ Recv(ms) ║ Total(s) ║ Status  ║   Error    ║
╠══════════╬══════════╬══════════╬══════════╬═════════╬════════════╣
║    100 B ║     16   ║    167   ║    0.2   ║   OK   ║        0   ║
║    256 B ║     30   ║    839   ║    0.9   ║   OK   ║        0   ║
║    512 B ║     52   ║    827   ║    0.9   ║   OK   ║        0   ║
║   1024 B ║    111   ║    848   ║    1.0   ║   OK   ║        0   ║
║   2048 B ║    212   ║   1127   ║    1.3   ║   OK   ║        0   ║
║   4096 B ║    410   ║     48   ║    0.5   ║   OK   ║        0   ║
║   8192 B ║    813   ║   1055   ║    1.9   ║   OK   ║        0   ║
║  16384 B ║   1623   ║   2585   ║    4.2   ║   OK   ║        0   ║
╚══════════╩══════════╩══════════╩══════════╩═════════╩════════════╝
  Connect: 15.4 s  Passed: 8/8  Failed: 0/8
```

### Payload Tests (Run 2)

```
╔══════════════════════════════════════════════════════════════════╗
║               WSS Stress Test Results                           ║
╠══════════╦══════════╦══════════╦══════════╦═════════╦════════════╣
║  Size    ║ Send(ms) ║ Recv(ms) ║ Total(s) ║ Status  ║   Error    ║
╠══════════╬══════════╬══════════╬══════════╬═════════╬════════════╣
║    100 B ║     16   ║    170   ║    0.2   ║   OK   ║        0   ║
║    256 B ║     30   ║    829   ║    0.9   ║   OK   ║        0   ║
║    512 B ║     52   ║    847   ║    0.9   ║   OK   ║        0   ║
║   1024 B ║    111   ║    987   ║    1.1   ║   OK   ║        0   ║
║   2048 B ║    212   ║    977   ║    1.2   ║   OK   ║        0   ║
║   4096 B ║    413   ║     55   ║    0.5   ║   OK   ║        0   ║
║   8192 B ║    815   ║   1894   ║    2.7   ║   OK   ║        0   ║
║  16384 B ║   1622   ║   2037   ║    3.7   ║   OK   ║        0   ║
╚══════════╩══════════╩══════════╩══════════╩═════════╩════════════╝
```

### Key Observations

- **8/8 tests pass** — all payload sizes from 100B to 16KB work correctly
- **Send scales linearly** with payload size (~0.1 ms/byte, UART-limited at 115200 baud)
- **Send formula:** `send_ms ≈ 16 + (payload_bytes × 0.098)` — predictable
- **Recv time is unpredictable** — dominated by cellular network latency + QSSLRECV polling jitter
- **4096 B anomaly** — recv is only ~50 ms because the 413 ms send time was long enough
  for the server to echo back the response before `gsm_ws_recv()` was called (data already
  in modem buffer → no polling delay). See "Explaining the 4096 B Anomaly" in Timing Breakdown.
- **Connect is slow** on first use (15.4s) — dominated by DNS + TLS handshake over cellular
- **Chunking kicks in** at 1024B+ (visible in logs as "Chunked send: N B total")
- **Test environment:** ESP-IDF v4.4.8, ESP32 rev 3.1, Quectel EC200U, Airtel 4G (India)

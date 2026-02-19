# HTTP/HTTPS Timing Phases

What happens inside each phase of an HTTPS POST request, explained with the actual AT commands and real timing from a 8KB test.

Enable/disable these logs with `GSM_LOG_HTTP_TIMING` in `gsm_private.h`.

---

## Real Log Output (Test 7/8 — 8192 bytes)

```
Phase 1 AT config:              117 ms
Phase 2 URL setup:             5034 ms
Phase 3 UART write (8341 B):   1453 ms
Phase 4 modem ACK:               39 ms
Phase 5 network RT:             639 ms  (DNS+TLS+upload+server)
Phase 6 read resp:               56 ms
─── TOTAL:                    12360 ms ───
```

---

## Phase 1: AT Config (117 ms)

**What it does:** Clean up previous session and configure the modem's HTTP engine.

```
ESP32 → AT+QHTTPSTOP          "Stop any old HTTP session"
ESP32 → (flush UART)           "Clear leftover data in the pipe"
ESP32 → (wait 100ms)           "Give modem time to finish cleanup"

ESP32 → AT+QHTTPCFG="contextid",1    "Use data connection #1"
Modem ← OK

ESP32 → AT+QHTTPCFG="sslctxid",1     "Use SSL context #1 (for HTTPS)"
Modem ← OK

ESP32 → AT+QHTTPCFG="requestheader",1  "I'll send raw HTTP headers"
Modem ← OK
```

**Why 117ms?** Three quick AT commands + 100ms flush delay. This is fast because
these are local modem config commands — no network involved.

---

## Phase 2: URL Setup (5034 ms)

**What it does:** Tell the modem which URL to POST to.

```
ESP32 → AT+QHTTPURL=23,10     "I'll send a 23-byte URL, wait up to 10s"
Modem ← CONNECT               "Ready, send the URL"   ← modem took ~5s here

ESP32 → https://rbaskets.in/GSM   (send the 23 URL bytes)
Modem ← OK                    "URL saved"
```

**Why 5034ms?** The modem waits up to 10 seconds after receiving `AT+QHTTPURL`
before replying `CONNECT`. In this test it took ~5 seconds. This delay is the
modem preparing its HTTP engine internally. On back-to-back requests this can
be slower because the modem is still cleaning up the previous HTTP session.

**Note:** This is NOT a network operation — the modem hasn't connected to the
server yet. It's just saving the URL internally.

---

## Phase 3: UART Write (1453 ms)

**What it does:** Push the HTTP headers + JSON body from ESP32 to modem over UART.

```
ESP32 → AT+QHTTPPOST=8341,10,60   "I'll POST 8341 bytes total"
Modem ← CONNECT                   "Ready, send the data"

ESP32 → POST /GSM HTTP/1.1\r\n                         ┐
ESP32 → Host: rbaskets.in\r\n                           │ Raw HTTP
ESP32 → Authorization: token 85edb...:84938...\r\n      │ headers
ESP32 → Content-Type: application/json\r\n              │ (149 bytes)
ESP32 → Content-Length: 8192\r\n                        │
ESP32 → \r\n                                            ┘
ESP32 → {"test":"stress","seq":7,"size":8192,"data":"AAA..."}  ← body (8192 bytes)
```

**Why 1453ms?** This is pure UART transfer time.
- Baud rate: 115200 bits/sec = ~11,520 bytes/sec
- Data sent: 8341 bytes (149 bytes headers + 8192 bytes body)
- Expected: 8341 / 11520 = **0.72 seconds**
- Actual: 1.45 seconds (overhead from UART framing, write calls, small delays)

**This phase scales with payload size:**
| Payload | Total bytes | Expected UART time | Actual |
|---------|------------|-------------------|--------|
| 100 B   | ~250 B     | ~22 ms            | ~50 ms |
| 1 KB    | ~1.2 KB    | ~100 ms           | ~200 ms |
| 8 KB    | ~8.3 KB    | ~720 ms           | ~1450 ms |
| 16 KB   | ~16.5 KB   | ~1430 ms          | ~2500 ms |

---

## Phase 4: Modem ACK (39 ms)

**What it does:** Wait for the modem to confirm it received all the POST data.

```
Modem ← OK    "Got all 8341 bytes, now I'll send them to the server"
```

**Why 39ms?** The modem just checks that it received the expected number of
bytes. This is almost instant — it's a local buffer check, no network involved.

---

## Phase 5: Network Round-Trip (639 ms)

**What it does:** The modem does everything on the network — this is the "real work."

```
Modem internally:
  1. DNS lookup      → resolve "rbaskets.in" to an IP address
  2. TCP connect     → open a connection to that IP
  3. TLS handshake   → negotiate encryption (HTTPS)
  4. Send POST data  → push the 8341 bytes over the encrypted connection
  5. Wait for server → server processes the request
  6. Receive reply   → server sends back HTTP response

Modem ← +QHTTPPOST: 0,200,0   "Done! Error=0, HTTP status=200, body=0 bytes"
```

**Why only 639ms here?** This was test 7 out of 8. By this point:
- DNS is **cached** from previous requests (no lookup needed)
- TLS session may be **resumed** (no full handshake needed)
- The server already has an active connection

**For the first request** this phase takes **7-9 seconds** because DNS + TLS
handshake must happen from scratch.

**Typical Phase 5 timing:**
| Request | Phase 5 time | Why |
|---------|-------------|-----|
| 1st request | 7000-9000 ms | Full DNS + TLS + TCP setup |
| 2nd request | 500-2000 ms | DNS cached, TLS may resume |
| 3rd+ request | 300-1000 ms | Everything cached/reused |

---

## Phase 6: Read Response (56 ms)

**What it does:** Ask the modem for the server's response body and read it.

```
ESP32 → AT+QHTTPREAD           "Give me the server's response"
Modem ← CONNECT
Modem ← (response body data)   "Here's what the server sent back"
Modem ← OK
Modem ← +QHTTPREAD: 0          "Read complete, no errors"
```

**Why 56ms?** The response body is already sitting in the modem's buffer (it
arrived during Phase 5). We're just reading it over UART — no network wait.
Small responses (few hundred bytes) transfer in under 100ms.

---

## Where Does the Time Go? (Visual)

```
0ms        117ms     5151ms                   6604ms  6643ms    7282ms  7338ms
 |          |          |                        |       |         |       |
 ├──────────┼──────────┼────────────────────────┼───────┼─────────┼───────┤
 │ Phase 1  │ Phase 2  │       Phase 3          │ Ph 4  │ Phase 5 │ Ph 6  │
 │ AT cfg   │ URL      │    UART write          │ ACK   │ Network │ Read  │
 │ 117ms    │ 5034ms   │    1453ms              │ 39ms  │ 639ms   │ 56ms  │
 │          │          │                        │       │         │       │
 │ local    │ modem    │  ESP32→modem           │ local │ INTERNET│ modem │
 │ config   │ internal │  serial transfer       │ check │ DNS+TLS │→ESP32 │
 └──────────┴──────────┴────────────────────────┴───────┴─────────┴───────┘
                                                                   TOTAL: 12360ms
```

---

## Summary

| Phase | What | Depends on | Typical time |
|-------|------|-----------|-------------|
| 1 | AT config | Number of config commands | 100-200 ms |
| 2 | URL setup | Modem internal readiness | 300-5000 ms |
| 3 | UART write | **Payload size** + baud rate | 50-2500 ms |
| 4 | Modem ACK | Almost nothing | 20-100 ms |
| 5 | Network | DNS + TLS + server + caching | 300-9000 ms |
| 6 | Read response | Response body size | 50-500 ms |

**Key takeaways:**
- **Phase 2** (URL setup) is surprisingly slow — modem internal processing
- **Phase 3** (UART write) is the only phase that grows with payload size
- **Phase 5** (network) is huge on first request but fast on subsequent ones (caching)
- **Phases 1, 4, 6** are always fast — local operations only

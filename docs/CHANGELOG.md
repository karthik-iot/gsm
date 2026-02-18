# Changelog

## HTTP Performance Optimization (2026-02-18)

### Problem

Every HTTPS POST took exactly **30 seconds** regardless of payload size (100B or 8KB).
Back-to-back requests failed with `-11` (CTX_ID) and `-13` (URL) errors due to
leftover UART data from the previous request.

### Root Cause Analysis

Two functions read modem UART data: `gsm_read_response()` and `gsm_collect_response()`.
Both only recognized `\r\nOK\r\n` and `\r\nERROR\r\n` as response terminators.

Quectel HTTP URCs like `+QHTTPPOST: 0,200,0\r\n` do **not** contain `OK`, so both
functions waited their full timeout before returning — even though the response had
already arrived.

**Time breakdown (before fix):**

```
gsm_expect_urc("+QHTTPPOST:", 20000ms)
  └─ gsm_read_response() receives "+QHTTPPOST: 0,200,0\r\n"
  └─ No "\r\nOK\r\n" found → waits full 20s timeout           ← 20s wasted

gsm_collect_response(AT+QHTTPREAD, 30000ms)
  └─ Receives body + OK + "+QHTTPREAD: 0"
  └─ Pattern matching fails on framing edge cases → waits     ← 10s wasted

Total per request: ~30s (should be ~5-8s)
```

### Changes Made

#### 1. `gsm_core.c` — `gsm_read_response()`: Added HTTP URC terminators

**Before:**
```c
if (strstr(buf, "\r\nOK\r\n") ||
    strstr(buf, "\r\nERROR\r\n") ||
    strstr(buf, "\r\n> ") ||
    strstr(buf, "+CME ERROR:")) {
    break;
}
```

**After:**
```c
if (strstr(buf, "\r\nOK\r\n") ||
    strstr(buf, "\r\nERROR\r\n") ||
    strstr(buf, "\r\n> ") ||
    strstr(buf, "+CME ERROR:") ||
    strstr(buf, "+QHTTPPOST:") ||
    strstr(buf, "+QHTTPGET:") ||
    strstr(buf, "+QHTTPREAD:")) {
    break;
}
```

**Why:** `gsm_expect_urc()` calls `gsm_read_response()` to wait for HTTP result URCs
like `+QHTTPPOST:`. Without recognizing these as terminators, it waited the full
timeout (20s) even though the URC had already been received and was sitting in the buffer.

---

#### 2. `gsm_core.c` — `gsm_collect_response()`: Added HTTP terminators + idle timeout

**Before:**
```c
// Only checked for \r\nOK\r\n and \r\nERROR\r\n
// No idle timeout — always waited full timeout_ms
```

**After:**
```c
// Added HTTP URC terminators
if (strstr(buf, "+QHTTPREAD:") || strstr(buf, "+QHTTPPOST:") || strstr(buf, "+QHTTPGET:")) {
    break;
}

// Added 500ms idle timeout — if data stops arriving, we're done
if (got_data && (time_since_last_rx > 500ms)) {
    break;
}
```

**Why:** This function reads the `AT+QHTTPREAD` response body. The modem sends data
in bursts. Once the full response arrives and 500ms passes with no new data, there's
no reason to keep waiting. The `+QHTTPREAD: 0` URC at the end is also now detected
as a clean terminator.

---

#### 3. `gsm_http.c` — `gsm_http_request()`: Added UART flush before each request

**Before:** No cleanup between requests.

**After:**
```c
gsm_flush_input(m);
vTaskDelay(pdMS_TO_TICKS(100));
```

**Why:** With faster response termination, there may be trailing modem data still in
the UART RX buffer (e.g., the `+QHTTPREAD: 0\r\n` URC that arrives after `OK`). If
not flushed, this stale data gets read by the next request's `AT+QHTTPCFG` command,
causing it to see garbage instead of `OK` — resulting in `-11` (CTX_ID) or `-13` (URL)
errors. The 100ms delay ensures the modem has finished sending any trailing data.

---

#### 4. `gsm_http.c` — Reduced AT+QHTTPPOST modem-side timeouts

**Before:** `AT+QHTTPPOST=%d,60,60`
**After:** `AT+QHTTPPOST=%d,10,30`

| Parameter | Before | After | Meaning |
|-----------|--------|-------|---------|
| Input timeout | 60s | 10s | Time modem waits for POST body data from ESP32 |
| Response timeout | 60s | 30s | Time modem waits for server HTTP response |

**Why:** The ESP32 sends POST data immediately after `CONNECT`. The modem doesn't need
60s to wait for input — 10s is more than enough. Server response timeout reduced to 30s
which is still generous for any reasonable endpoint.

---

#### 5. `gsm_http.c` — Reduced ESP32-side wait timeouts

| Wait for | Before | After | Reason |
|----------|--------|-------|--------|
| `OK` after POST data | 10s | 5s | Modem acknowledges data receipt almost instantly |
| `+QHTTPPOST:` URC | 20s | 15s | Network round-trip rarely exceeds 10s on LTE |
| `gsm_collect_response` max | 30s | 10s | Response body arrives within seconds |

**Why:** The original timeouts were set conservatively for worst-case scenarios. With
proper terminator detection, these are now just safety nets — the functions exit as
soon as the actual response arrives, not when the timeout expires.

---

### Results

**Before optimization:**
```
║    100 B ║     30s  ║   OK   ║
║   8192 B ║     30s  ║   OK   ║
```

**After optimization:**
```
║    100 B ║     10s  ║   OK   ║
║   8192 B ║     11s  ║   OK   ║
  Passed: 7/7
```

- HTTPS POST time: **30s → 10-11s** (3x faster)
- Back-to-back reliability: **1/7 → 7/7** (100% pass rate)
- Remaining ~10s is real network latency (TLS handshake + LTE round-trip)

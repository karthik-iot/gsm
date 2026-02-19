# GSM Modem: HTTPS POST Flow

How an ESP32 talks to a Quectel EC200U modem to make an HTTPS POST request over 4G/LTE.

---

## Flow Diagram

```
  ESP32                          Quectel EC200U                    Internet
  -----                          --------------                    --------

  +=========================================================================+
  |  PHASE 1: WAKE UP & HANDSHAKE                                          |
  +=========================================================================+
  |                                                                         |
  |  AT ---------------------->  "Are you there?"                           |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  ATI --------------------->  "Tell me your name"                        |
  |  <------------------------ Quectel EC200U / OK                          |
  |                                                                         |
  |  ATV1 -------------------->  "Give me readable replies"                 |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  ATE0 -------------------->  "Stop repeating what I say"                |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  AT+CMEE=2 --------------->  "Give me detailed error messages"          |
  |  <------------------------ OK                                           |
  |                                                                         |
  +=========================================================================+

  +=========================================================================+
  |  PHASE 2: CHECK IDENTITY & SIM                                          |
  +=========================================================================+
  |                                                                         |
  |  AT+GSN ------------------>  "What's your IMEI number?"                 |
  |  <------------------------ 86XXXXXXXXXXXXX / OK                         |
  |                                                                         |
  |  AT+CPIN? ---------------->  "Is the SIM card inserted & unlocked?"     |
  |  <------------------------ +CPIN: READY / OK                            |
  |                                                                         |
  |  AT+CIMI ----------------->  "What's the SIM's IMSI number?"            |
  |  <------------------------ 40XXXXXXXXXXXXX / OK                         |
  |                                                                         |
  |  AT+QCCID ---------------->  "What's the SIM card ID (ICCID)?"         |
  |  <------------------------ +QCCID: 89XXXXXXXXXX / OK                    |
  |                                                                         |
  +=========================================================================+

  +=========================================================================+
  |  PHASE 3: CHECK SIGNAL & NETWORK                                        |
  +=========================================================================+
  |                                                                         |
  |  AT+CSQ ------------------>  "How strong is the signal?"                |
  |  <------------------------ +CSQ: 18,0 / OK     (18 out of 31)          |
  |                                                                         |
  |  AT+CREG? ---------------->  "Are we registered on 2G/3G?"             |
  |  <------------------------ +CREG: 0,1 / OK     (1 = registered)        |
  |                                                                         |
  |  AT+CEREG? --------------->  "Are we registered on 4G/LTE?"            |
  |  <------------------------ +CEREG: 0,1 / OK    (1 = registered)        |
  |                                                                         |
  |  AT+COPS? ---------------->  "Which operator are we connected to?"      |
  |  <------------------------ +COPS: 0,0,"Jio" / OK                       |
  |                                                                         |
  |     +--------------------------------------------+                      |
  |     |  If not registered, wait & retry every     |                      |
  |     |  2 seconds for up to 60 seconds            |                      |
  |     +--------------------------------------------+                      |
  |                                                                         |
  +=========================================================================+

  +=========================================================================+
  |  PHASE 4: CONNECT TO INTERNET (GPRS/DATA)                               |
  +=========================================================================+
  |                                                                         |
  |  AT+CGATT? --------------->  "Is data/GPRS turned on?"                  |
  |  <------------------------ +CGATT: 1 / OK      (1 = yes)               |
  |                                                                         |
  |  AT+CGATT=1 -------------->  "Turn on data if not already on"           |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  AT+CGDCONT=1,"IP",        "Set APN to jionet -- this tells the        |
  |    "jionet" -------------->  modem which mobile network gateway         |
  |  <------------------------ OK  to use for internet"                     |
  |                                                                         |
  |  AT+QICSGP=1,1,"jionet",  "Configure the data connection with          |
  |    "","",0 --------------->  APN, username, password, auth type"        |
  |  <------------------------ OK                                           |
  |                                                                         |
  +=========================================================================+

  +=========================================================================+
  |  PHASE 5: ACTIVATE DATA CONNECTION (PDP CONTEXT)                        |
  +=========================================================================+
  |                                                                         |
  |  AT+QIACT=1 ------------->  "Open the data pipe -- get an              |
  |  <------------------------ OK  IP address from the network"             |
  |                                                                         |
  |     At this point the modem has an IP address                           |
  |     and can reach the internet.                                         |
  |                                                                         |
  +=========================================================================+

  +=========================================================================+
  |  PHASE 6: HTTPS POST (with custom headers)                              |
  +=========================================================================+
  |                                                                         |
  |  6a. Stop previous session & configure HTTP                             |
  |  ------------------------------------------                             |
  |  AT+QHTTPSTOP ----------->  "Cancel any lingering HTTP session"         |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  AT+QHTTPCFG=              "Use data connection #1 for HTTP"            |
  |    "contextid",1 -------->                                              |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  AT+QHTTPCFG=              "Use SSL context #1 for HTTPS"               |
  |    "sslctxid",1 --------->                                              |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  AT+QHTTPCFG=              "I will provide raw HTTP headers             |
  |    "requestheader",1 --->   in the POST body"                           |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  6b. Set the URL                                                        |
  |  ----------------                                                       |
  |  AT+QHTTPURL=60,10 ----->  "I'm going to send you a URL that           |
  |  <------------------------ CONNECT  is 60 characters long"              |
  |                                                                         |
  |  https://demo.iotready    (send the actual URL bytes)                   |
  |  .co/api/method/otp.api                                                 |
  |  .insert_iot_event ------>                                              |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  6c. Send POST data (headers + body)                                    |
  |  -----------------------------------                                    |
  |  AT+QHTTPPOST=             "I'm going to POST <total> bytes.            |
  |    <total>,10,60 -------->  total = header_block + json_body"           |
  |  <------------------------ CONNECT                                      |
  |                                                                         |
  |  POST /api/method/otp.     ┐                                            |
  |   api.insert_iot_event     │  Raw HTTP headers prepended                |
  |   HTTP/1.1\r\n             │  to the body when                          |
  |  Host: demo.iotready.     │  requestheader=1                           |
  |   co\r\n                   │                                            |
  |  Authorization: token      │  The modem sends these                     |
  |   85edb...:84938...\r\n    │  as-is to the server.                     |
  |  Content-Type:             │                                            |
  |   application/json\r\n     │                                            |
  |  Content-Length: 100\r\n   │                                            |
  |  \r\n  ------------------->┘                                            |
  |                                                                         |
  |  {"key":"value",...} ----->  (JSON body bytes)            +----------+  |
  |  <------------------------ OK  (modem accepted the data)  |          |  |
  |                                                           |  Server  |  |
  |     ... modem sends HTTPS request to server ...  -------->| receives |  |
  |     ... modem receives server response ...       <--------| & replies|  |
  |                                                           |          |  |
  |  <------------------------ +QHTTPPOST: 0,200,0            +----------+  |
  |                             |  |   |                                    |
  |                             |  |   +-- response body length             |
  |                             |  +------ HTTP status (200 = OK)           |
  |                             +--------- error code (0 = success)         |
  |                                                                         |
  |  6d. Read the response body                                             |
  |  --------------------------                                             |
  |  AT+QHTTPREAD ----------->  "Give me what the server sent back"         |
  |  <------------------------ CONNECT                                      |
  |  <------------------------ {"status":"ok","id":"abc123"}                |
  |  <------------------------ OK                                           |
  |  <------------------------ +QHTTPREAD: 0   (read complete)             |
  |                                                                         |
  |  AT+QHTTPCFG=              "Reset back to automatic headers"            |
  |    "requestheader",0 --->                                               |
  |  <------------------------ OK                                           |
  |                                                                         |
  +=========================================================================+

  +=========================================================================+
  |  PHASE 7: CLEANUP                                                       |
  +=========================================================================+
  |                                                                         |
  |  AT+QIDEACT=1 ----------->  "Close the data connection"                |
  |  <------------------------ OK                                           |
  |                                                                         |
  +=========================================================================+
```

---

## AT Command Reference

Every command below is something the ESP32 sends to the modem over UART (serial).
The modem replies with `OK`, `ERROR`, or data.

### Phase 1: Wake Up & Handshake

| Command | What it does (simple) |
|---------|----------------------|
| `AT` | **Ping the modem.** Like saying "hello, are you there?" -- the modem replies `OK` if alive. We try this 10 times until it wakes up. |
| `ATI` | **Get modem info.** Returns the modem name (e.g., "Quectel EC200U") and firmware version. Just for logging. |
| `ATV1` | **Verbose mode ON.** Makes the modem reply with words like `OK` and `ERROR` instead of just numbers `0` and `4`. Easier to read. |
| `ATE0` | **Turn off echo.** Without this, the modem repeats back every command we send. With echo off, it only sends replies. Cleaner to parse. |
| `AT+CMEE=2` | **Detailed errors.** Instead of just `ERROR`, the modem will say things like `+CME ERROR: SIM not inserted` -- much easier to debug. |
| `AT+IPR?` | **Check baud rate.** Asks "what speed are you talking at?" Should match our UART config (115200). |

### Phase 2: Check Identity & SIM

| Command | What it does (simple) |
|---------|----------------------|
| `AT+GSN` | **Get IMEI.** The modem's unique hardware ID (like a serial number). Every modem in the world has a different one. 15 digits. |
| `AT+CPIN?` | **Check SIM status.** Is the SIM card inserted and unlocked? Reply `READY` = good. `SIM PIN` = needs PIN code. No reply = no SIM. |
| `AT+CIMI` | **Get IMSI.** The SIM card's subscriber ID. This identifies your mobile plan/account. |
| `AT+QCCID` | **Get ICCID.** The SIM card's physical ID number (printed on the SIM). Useful for identifying which SIM card is inserted. |

### Phase 3: Signal & Network

| Command | What it does (simple) |
|---------|----------------------|
| `AT+CSQ` | **Signal strength.** Returns a number from 0-31. Higher = better. Below 10 = weak signal, might have trouble with data. 99 = no signal at all. |
| `AT+CREG?` | **2G/3G registration.** Is the modem registered on the mobile network? Second number: 0=searching, 1=registered (home), 5=registered (roaming). |
| `AT+CGREG?` | **GPRS registration.** Same as above but specifically for data (GPRS/2G data). |
| `AT+CEREG?` | **4G/LTE registration.** Same check but for 4G. This is what we mainly use since Jio is 4G only. |
| `AT+COPS?` | **Current operator.** Which mobile company are we connected to? Returns name like "Jio 4G" or "Airtel". |

### Phase 4: Connect to Internet

| Command | What it does (simple) |
|---------|----------------------|
| `AT+CGATT?` | **Check data attachment.** Is the modem connected to the mobile data network? 1 = yes, 0 = no. |
| `AT+CGATT=1` | **Attach to data.** Turn on mobile data. Like switching on "mobile data" on your phone. |
| `AT+CGDCONT?` | **Read current APN.** Queries the modem for the APN already configured by the SIM/network. Response: `+CGDCONT: 1,"IP","airtelgprs.com",...`. Used by `gsm_get_apn()` to auto-detect the APN so you can swap SIMs without changing code. |
| `AT+CGDCONT=1,"IP","<apn>"` | **Set APN.** APN = Access Point Name. It's like a gateway address that your SIM provider gives you. For Jio it's `jionet`. For Airtel it's `airtelgprs.com`. Without the right APN, you can't get internet. Used as fallback if auto-detect returns nothing. |
| `AT+QICSGP=1,1,"<apn>","","",0` | **Configure data connection.** Sets the APN along with username, password, and auth type. For most Indian carriers there's no username/password needed (empty strings) and no auth (0). |

### Phase 5: Open Data Pipe (PDP Context)

| Command | What it does (simple) |
|---------|----------------------|
| `AT+QIACT=1` | **Activate PDP context.** This is the final step to get internet. The modem asks the network for an IP address (like getting a seat at the internet table). After this, the modem can talk to websites. |
| `AT+QIACT?` | **Check if active.** Shows current PDP contexts and their IP addresses. Useful to verify the connection is working. |
| `AT+QIDEACT=1` | **Deactivate PDP.** Close the data connection. Like logging off from the internet. Done at cleanup. |

### Phase 6: HTTPS POST

| Command | What it does (simple) |
|---------|----------------------|
| `AT+QHTTPSTOP` | **Stop HTTP session.** Cancels any lingering HTTP request. Prevents `CME ERROR: 711` (HTTP busy) on back-to-back requests. Sent before every new request. |
| `AT+QHTTPCFG="contextid",1` | **Link HTTP to data connection.** Tells the HTTP engine "use data connection #1 for your requests." |
| `AT+QHTTPCFG="sslctxid",1` | **Link HTTPS to SSL.** Tells the HTTP engine to use SSL context #1 for encryption. This is what makes it HTTPS (secure) instead of HTTP. |
| `AT+QHTTPCFG="requestheader",1` | **Custom headers ON.** Tells the modem "I'll provide raw HTTP headers prepended to the POST body." **Important:** `AT+QHTTPCFG="header"` is NOT a valid command — you must include headers in the POST data itself. |
| `AT+QHTTPCFG="requestheader",0` | **Custom headers OFF.** Reset back to automatic headers. Sent after each request completes (or fails). |
| `AT+QHTTPURL=<len>,10` | **Set URL.** Tells the modem "I'm about to send a URL that is `<len>` bytes long. Wait up to 10 seconds for me to send it." Modem replies `CONNECT`, then we send the URL bytes. |
| `AT+QHTTPPOST=<len>,10,60` | **Start POST.** `<len>` = total bytes (headers + body when `requestheader=1`, or just body when `requestheader=0`). Wait 10s for input, 60s for server reply. Modem says `CONNECT`, we send the data. |
| _+QHTTPPOST: 0,200,0_ | **POST result (URC).** This is the modem telling us the result. `0` = no error, `200` = HTTP 200 OK (success), `0` = response body length. This comes automatically -- we don't send it. |
| `AT+QHTTPREAD` | **Read response body.** "Give me whatever the server sent back." The modem sends the response data followed by `+QHTTPREAD: 0` when done. |
| `AT+QHTTPGET=60` | **Start GET** (for reference). Like POST but simpler -- just downloads a page. The 60 means wait up to 60s for the server. |

#### How custom headers work (requestheader=1)

When `requestheader=1`, the data sent via `AT+QHTTPPOST` must include raw HTTP headers **prepended** to the body:

```
POST /api/endpoint HTTP/1.1\r\n        ← request line
Host: example.com\r\n                  ← required
Authorization: token xxx:yyy\r\n       ← custom header
Content-Type: application/json\r\n     ← custom header
Content-Length: 42\r\n                 ← body size
\r\n                                   ← empty line = end of headers
{"key":"value"}                        ← actual JSON body
```

The `<len>` in `AT+QHTTPPOST=<len>` is the **total** byte count (all headers + separator + body).
The URL from `AT+QHTTPURL` is still used for the TCP/TLS connection, but the request line in the headers tells the server which path to hit.

### Other Useful Commands

| Command | What it does (simple) |
|---------|----------------------|
| `AT+CFUN=1,1` | **Reboot modem.** Like pulling the power cord and plugging it back in. Takes about 5 seconds to come back up. |
| `AT+QPOWD=1` | **Power off modem.** Shut down cleanly. |

---

## What is a URC?

**URC = Unsolicited Result Code**

Normally the modem only talks when you ask it something (send a command, get a reply).
But sometimes the modem speaks on its own to tell you something happened. These are URCs.

Examples:
- `+QHTTPPOST: 0,200,0` -- "The HTTP POST finished, here's the result"
- `+QHTTPREAD: 0` -- "I'm done sending you the response body"
- `+QHTTPGET: 0,200,1234` -- "The HTTP GET finished"

You don't send these -- you **wait** for them. The ESP32 watches the UART for these
messages after sending a command.

---

## What is PDP Context?

Think of it like this:
- **SIM card** = your membership card to the mobile network
- **Network registration** = showing your card at the door
- **GPRS attach** = entering the building
- **PDP context activation** = sitting down at a desk and getting a computer (IP address)

Only after PDP activation can you actually browse the internet, make HTTP requests, etc.

---

## Timing (what we measured)

| Step | Time |
|------|------|
| Modem wake up (AT sync) | 1-5 seconds |
| SIM check | 1-20 seconds |
| Network registration | 2-60 seconds |
| GPRS attach | 1-10 seconds |
| PDP activation | 1-15 seconds |
| **HTTPS POST (100B-2KB)** | **10-11 seconds** |
| **HTTPS POST (4KB-8KB)** | **11-12 seconds** |
| **HTTPS POST (16KB)** | **13 seconds** |

The HTTPS POST time includes:
- TLS/SSL handshake (encryption setup) -- ~3-4s
- DNS resolution -- ~1s
- Data upload over LTE -- ~1-2s (scales with payload size)
- Server processing -- ~1s
- Response download -- ~1s

### Why does POST time increase with payload size?

The base ~10s is **fixed overhead** — the same regardless of payload size:

```
┌─────────────────────────────────────────────────────┐
│          HTTPS POST Time Breakdown (16KB)            │
├─────────────────────────────┬───────────────────────┤
│  Phase                      │  Time                 │
├─────────────────────────────┼───────────────────────┤
│  DNS resolution             │  ~1s       (fixed)    │
│  TLS/SSL handshake          │  ~3-4s     (fixed)    │
│  LTE radio + TCP setup      │  ~1-2s     (fixed)    │
│  AT command round-trips     │  ~2-3s     (fixed)    │
│  UART: ESP32 → modem        │  ~1.4s     (scales)   │
│  TLS encrypt + LTE upload   │  ~1-2s     (scales)   │
│  Server receive + respond   │  ~1s       (scales)   │
├─────────────────────────────┼───────────────────────┤
│  TOTAL                      │  ~13s                 │
└─────────────────────────────┴───────────────────────┘
```

**Fixed overhead (~10s)** — dominates the total time:
- DNS lookup, TLS handshake, and TCP connection setup happen once per request
- AT command exchanges (QHTTPCFG, QHTTPURL, QHTTPPOST, QHTTPREAD) add ~2-3s of UART back-and-forth
- Even a 100-byte POST takes ~10s because of this setup cost

**Scaling portion (+1-3s for larger payloads):**
- **UART transfer:** At 115200 baud (~11.5 KB/s), pushing 16KB from ESP32 to modem takes ~1.4s. For 100B it's <10ms — negligible.
- **TLS encryption:** The modem's CPU must encrypt larger payloads, and may split them across multiple TLS records.
- **LTE upload:** More data = more radio frames over the air interface.

This is why the time scaling is very gentle — only **+3s for a 160x increase** in payload size (100B → 16KB). The connection setup dominates, not the data transfer.

### Stress test results (with custom headers, 8/8 pass)

```
║    100 B ║     10s  ║   OK   ║
║    256 B ║     10s  ║   OK   ║
║    512 B ║     10s  ║   OK   ║
║   1024 B ║     10s  ║   OK   ║
║   2048 B ║     11s  ║   OK   ║
║   4096 B ║     11s  ║   OK   ║
║   8192 B ║     12s  ║   OK   ║
║  16384 B ║     13s  ║   OK   ║
  Passed: 8/8
```

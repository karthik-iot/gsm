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
  |  PHASE 6: HTTPS POST                                                    |
  +=========================================================================+
  |                                                                         |
  |  6a. Configure HTTP                                                     |
  |  ------------------                                                     |
  |  AT+QHTTPCFG=              "Use data connection #1 for HTTP"            |
  |    "contextid",1 -------->                                              |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  AT+QHTTPCFG=              "Use SSL context #1 for HTTPS"               |
  |    "sslctxid",1 --------->                                              |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  6b. Set the URL                                                        |
  |  ----------------                                                       |
  |  AT+QHTTPURL=25,10 ----->  "I'm going to send you a URL that           |
  |  <------------------------ CONNECT  is 25 characters long"              |
  |                                                                         |
  |  https://rbaskets.in/GSM >  (send the actual URL bytes)                 |
  |  <------------------------ OK                                           |
  |                                                                         |
  |  6c. Send POST data                                                     |
  |  -------------------                                                    |
  |  AT+QHTTPPOST=             "I'm going to POST 100 bytes.                |
  |    100,10,30 ------------>  Wait 10s for my data, 30s for server"       |
  |  <------------------------ CONNECT                                      |
  |                                                                         |
  |  {"test":"stress",         (send the JSON body bytes)                    |
  |   "data":"AAA..."} ------>                                +----------+  |
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
| `AT+CGDCONT=1,"IP","jionet"` | **Set APN.** APN = Access Point Name. It's like a gateway address that your SIM provider gives you. For Jio it's `jionet`. For Airtel it's `airtelgprs.com`. Without the right APN, you can't get internet. |
| `AT+QICSGP=1,1,"jionet","","",0` | **Configure data connection.** Sets the APN along with username, password, and auth type. For Jio there's no username/password needed (empty strings) and no auth (0). |

### Phase 5: Open Data Pipe (PDP Context)

| Command | What it does (simple) |
|---------|----------------------|
| `AT+QIACT=1` | **Activate PDP context.** This is the final step to get internet. The modem asks the network for an IP address (like getting a seat at the internet table). After this, the modem can talk to websites. |
| `AT+QIACT?` | **Check if active.** Shows current PDP contexts and their IP addresses. Useful to verify the connection is working. |
| `AT+QIDEACT=1` | **Deactivate PDP.** Close the data connection. Like logging off from the internet. Done at cleanup. |

### Phase 6: HTTPS POST

| Command | What it does (simple) |
|---------|----------------------|
| `AT+QHTTPCFG="contextid",1` | **Link HTTP to data connection.** Tells the HTTP engine "use data connection #1 for your requests." |
| `AT+QHTTPCFG="sslctxid",1` | **Link HTTPS to SSL.** Tells the HTTP engine to use SSL context #1 for encryption. This is what makes it HTTPS (secure) instead of HTTP. |
| `AT+QHTTPURL=<len>,10` | **Set URL.** Tells the modem "I'm about to send a URL that is `<len>` bytes long. Wait up to 10 seconds for me to send it." Modem replies `CONNECT`, then we send the URL bytes. |
| `AT+QHTTPPOST=<len>,10,30` | **Start POST.** "I'm going to send `<len>` bytes of data. Wait 10s for my data, then wait 30s for the server to reply." Modem says `CONNECT`, we send the data, then we wait for the result. |
| _+QHTTPPOST: 0,200,0_ | **POST result (URC).** This is the modem telling us the result. `0` = no error, `200` = HTTP 200 OK (success), `0` = response body length. This comes automatically -- we don't send it. |
| `AT+QHTTPREAD` | **Read response body.** "Give me whatever the server sent back." The modem sends the response data followed by `+QHTTPREAD: 0` when done. |
| `AT+QHTTPGET=60` | **Start GET** (for reference). Like POST but simpler -- just downloads a page. The 60 means wait up to 60s for the server. |
| `AT+QHTTPCFG="requestheader",1` | **Custom headers ON.** Tells the modem "I'll provide my own HTTP headers." We don't use this for our stress test (modem handles headers automatically). |
| `AT+QHTTPCFG="requestheader",0` | **Custom headers OFF.** Reset back to automatic headers after each request. |

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
| **Single HTTPS POST** | **10-11 seconds** |

The HTTPS POST time includes:
- TLS/SSL handshake (encryption setup) -- ~3-4s
- DNS resolution -- ~1s
- Data upload over LTE -- ~1-2s
- Server processing -- ~1s
- Response download -- ~1s

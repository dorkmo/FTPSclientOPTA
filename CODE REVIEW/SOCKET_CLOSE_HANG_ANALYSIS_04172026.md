# ArduinoOPTA-FTPS — Mbed Socket Close-Path Hang Analysis

**Date:** 2026-04-17
**Affected file:** `src/transport/MbedSecureSocketFtpsTransport.cpp`
**Hardware:** Arduino Opta (STM32H747, dual-core), mbed_opta core 4.5.0
**mbed-os branch shipped with core:** mbed-os-6.17.x equivalent
**Test server:** Pure-FTPd (WD My Cloud OS 5), also reproduced with pyftpdlib

This document describes an open problem with FTPS data-channel teardown
on Mbed OS that partially defeats multi-file FTPS backup on the
Arduino Opta. It is intended as a standalone hand-off: a reader who has
never seen the codebase should be able to understand the symptom, the
evidence, what has been tried, and what is still worth trying.

---

## 1. Problem statement

### 1.1 Desired behaviour

A single FTPS session should be able to upload N files in sequence:

```
connect/AUTH TLS → login → (PASV → STOR file1) → (PASV → STOR file2) → … → QUIT
```

### 1.2 Observed behaviour

Exactly **one** file uploads per session. The second `PASV`/data-channel
`connect()` either fails immediately or the server refuses the new data
connection. The client then reports:

```
FTPS session dropped; aborted remaining uploads
```

The HTTP endpoint that drives the backup now returns `HTTP 200` with
`filesUploaded: 1, filesFailed: N-1`. Previously it hung for 60–90 s
and tripped the 30 s watchdog, resetting the board — that **primary
hang** is fixed, this document is about the remaining limitation.

### 1.3 Root of the remaining limitation

To avoid the watchdog-resetting hang, the close path in
`MbedSecureSocketFtpsTransport` no longer calls `close()` or `delete`
on the TLS wrapper or TCP socket. It only:

1. flips the `TCPSocket` to non-blocking,
2. sends `mbedtls_ssl_close_notify()` directly, and
3. nulls the pointers.

This **leaks one LWIP socket handle per data transfer**. Mbed's LWIP
socket table is small (`MEMP_NUM_NETCONN`, default 8); after 1–2
transfers the Opta cannot allocate a new TCP socket for the next PASV
cycle, so the second file fails.

---

## 2. Why we can't just call `close()` / `delete`

This is the core obstacle. Every natural teardown path re-enters the
same blocking code in Mbed OS that caused the original watchdog-reset
bug.

### 2.1 The underlying Mbed bug

`TLSSocketWrapper::close()` and `TCPSocket::close()` **do not honor
`set_timeout()` or `set_blocking(false)`** during the TLS
`close_notify` + TCP `FIN` exchange. With certain FTPS peers they
block 30–90 s waiting for the peer side to complete the four-way
shutdown, even when the application has asked for non-blocking mode.

Observed on Opta with mbed_opta 4.5.0, against both Pure-FTPd
(WD My Cloud OS 5) and pyftpdlib.

### 2.2 Cascading consequences

| Attempted teardown | What blocks |
|---|---|
| `tls->close()`            | Re-enters `mbedtls_ssl_close_notify` + `tcp->close()`; blocks. |
| `delete tls`              | Destructor calls `close()` → same blocking path. |
| `tcp->close()` (only)     | LWIP `close` path blocks while `TLSSocketWrapper` still holds `mbedtls` BIO callbacks into the TCP socket. |
| `delete tcp` (only)       | Destructor calls `close()` → same blocking path. |
| `mbedtls_ssl_close_notify(ctx)` **directly** | Safe — it writes the alert through the already-open BIO and returns. Does **not** free any Mbed resources though. |

We confirmed cases 2–5 on real hardware; each path produced a
reproducible boot-time hang that required a recovery flash.

---

## 3. Timeline and evidence

Millisecond timestamps below are from an in-RAM trace buffer written by
the FTPS client's `tracePhase()` callback. `xport:` entries are emitted
by the transport layer; `store:` entries by `FtpsClient::store()`.

### 3.1 Before any fix — watchdog reset

```
store:data-close
xport:data:close-enter   ← never exits; watchdog fires ~30 s later
```

The device reboots mid-transfer. Every subsequent boot starts clean.

### 3.2 After abandon-only + close_notify fix (current state)

```
store:final-reply-read(51729)   store asks server for the 226 reply
store:final-reply-got (52099)   server responds in ~370 ms
                                 (was 15,400 ms before close_notify fix)
store:done                      first file succeeds
store:entry                     start of second file
store:pasv
store:data-open
xport:data:close-enter   (tear down the previous data socket shell)
xport:data:close-done
xport:data:tcp-connecting
quit:send                ← PASV data connect failed; client gives up
```

The elapsed time inside the problematic `tcp-connecting` step is
sub-second; the failure mode is "allocate failed" or "SYN rejected",
not a hang.

### 3.3 HTTP layer view

```
POST /api/ftp-backup
→ HTTP 200 in ~29 s
  { "ok": true, "filesUploaded": 1, "filesFailed": 1,
    "message": "FTPS session dropped; aborted remaining uploads",
    "failedFiles": "contacts_config.json" }
```

No watchdog. No reboot. Device remains responsive. Exactly one file
per session uploads.

---

## 4. Code location and current implementation

```cpp
// src/transport/MbedSecureSocketFtpsTransport.cpp  (namespace-local)

void ftpsReleaseSocketPair(TLSSocketWrapper *&tls, TCPSocket *&tcp) {
    // (a) force non-blocking so nothing below can stall
    if (tcp != nullptr) {
        tcp->set_blocking(false);
        tcp->set_timeout(0);
    }

    // (b) send TLS close_notify directly (bypasses Mbed close path)
    if (tls != nullptr) {
        mbedtls_ssl_context *ssl = tls->get_ssl_context();
        if (ssl != nullptr) {
            (void)mbedtls_ssl_close_notify(ssl);
        }
    }

    // (c) Abandon. Every close()/delete tried blocks — see section 2.2.
    (void)tls;
    (void)tcp;
    tls = nullptr;
    tcp = nullptr;
}
```

Callers:

- `MbedSecureSocketFtpsTransport::closeData()` — trace
  `xport:data:close-enter` / `xport:data:close-done`.
- `MbedSecureSocketFtpsTransport::closeControl()` — trace
  `xport:control:close-enter` / `xport:control:close-done`.

Allocation in `openDataChannel()`/`upgradeDataToTls()`:

```cpp
_dataSocket = new (std::nothrow) TCPSocket;
_dataSocket->open(_network);
_dataSocket->connect(address);
_dataTls = new (std::nothrow) TLSSocketWrapper(
    _dataSocket, serverName, TLSSocketWrapper::TRANSPORT_KEEP);
_dataTls->connect();           // TLS handshake
```

`TRANSPORT_KEEP` was chosen specifically so the TLS wrapper does **not**
call `close()` on the TCP socket in its own destructor — we manage the
TCP socket ourselves. That choice is still correct for a working
teardown; the problem is that our own teardown is the thing blocking.

---

## 5. Attempts that have been made

Grouped by strategy. All were tested live on hardware unless noted.

### 5.1 Respecting `set_timeout` / `set_blocking` on `close()`

| Variant | Result |
|---|---|
| `tls->set_timeout(3000); tls->close();`                    | blocks 30+ s, watchdog resets board |
| `tls->set_blocking(false); tls->set_timeout(500); tls->close();` | blocks, watchdog |
| Close inner `tcp` first to break the TLS BIO, then `tls`   | blocks at the first `close()` |
| `delete tls` with no explicit close                         | destructor invokes same close path, hangs |

Conclusion: **Mbed's socket close path is not honoring our
non-blocking/timeout request.** This is consistent across MbedSocket
and LWIP layers in mbed_opta 4.5.0.

### 5.2 Offload teardown to another thread

Tried a detached CMSIS thread (`osThreadNew` with
`osThreadDetached`) that owns the `delete`.

Result: thread creation itself serialized on an internal mutex held by
the previously stuck cleanup thread, re-blocking the caller. This only
made the blocking behaviour more confusing.

Not attempted yet:
- `mbed::EventQueue` dispatched to a persistent worker thread (not a
  fresh detached one each time).
- Pushing teardown to an `mbed::LowPowerTicker` / `Ticker` callback
  that runs in ISR context. May not be usable because Mbed socket APIs
  aren't ISR-safe.

### 5.3 Direct `mbedtls_ssl_close_notify()` only (current)

Works and is the reason the primary hang is gone.
`TLSSocketWrapper::get_ssl_context()` exposes the `mbedtls_ssl_context*`
publicly in Mbed OS 6; this lets us send the TLS alert without
entering the blocking wrapper.

Result: server sees a clean shutdown, emits `226` promptly, the
application returns. **Does not release any resources**, hence the
one-file limit.

### 5.4 Partial release: `tcp->close()` after `close_notify`

Intent: release the LWIP handle (the scarce resource) even if we leak
the `TLSSocketWrapper` heap (~4 KB).

Result: **device hangs at boot** after the very first backup tear-down.
A recovery flash was needed. Hypothesis: LWIP `close` path still
synchronizes on socket state that `mbedtls`' BIO callbacks reach into
from the still-live `TLSSocketWrapper`, because those BIO callbacks
are invoked when `close` triggers a final `_transport->send()` (e.g.
for a `RST`).

Reverted.

### 5.5 Full release: `delete tls; delete tcp`

Identical outcome to 5.4. Device hangs at boot, recovery flash needed.
Reverted.

### 5.6 Unexplored — candidate fixes

Listed roughly in increasing difficulty.

1. **Bump `MEMP_NUM_NETCONN`** in `mbed_app.json` so the leak is
   survivable for the expected number of files per session. Doesn't
   fix the leak, just moves the failure threshold. Cheap to try.

2. **Pre-allocate a single `TCPSocket` in the transport ctor** and
   re-open it between PASV cycles (`tcp->open(_network)` →
   `tcp->connect()` → transfer → `tcp->close()` (but still blocks?)).
   The question is whether `close()` on a *fully* established TLS
   session blocks differently than on the Mbed BIO-wrapped one.
   Likely yes, because this is the plain path the rest of Mbed uses.

3. **Per-file control reconnect.** Instead of trying to reuse the
   control channel across files, do `QUIT` + `AUTH TLS` + login for
   every file. Forces the server into a fresh state and sidesteps the
   PASV-refused behaviour. Cost: one full handshake per file (~0.7 s
   on Opta with cached session tickets). Fastest to implement — no
   Mbed changes, just a policy switch in `FtpsClient::store()` or the
   Opta-side backup orchestrator.

4. **Patch Mbed OS.** Make `TCPSocket::close()` honour
   `set_timeout()`. This is the Right Fix but requires either a local
   patch to `mbed_opta/4.5.0/cores/arduino/mbed/connectivity/...` or
   an upstream PR. Scope should be limited to the close path; don't
   touch send/recv.

5. **Bypass `TLSSocketWrapper` entirely.** Drive `mbedtls_ssl_context`
   ourselves over a `TCPSocket` BIO we own. Gives complete control
   over close ordering, at the cost of reimplementing a few hundred
   lines of `TLSSocketWrapper`.

6. **Post-abandon reclaim.** Keep abandon as today, but on the next
   `open()` attempt detect failure and explicitly walk the LWIP
   `netconn` table (internal Mbed API) to close any stale handles
   owned by our process. Fragile and version-dependent.

---

## 6. What reviewers should verify / try

1. **Confirm the Mbed close-blocks-on-FTPS pattern is not specific to
   this server.** Repro on a third FTPS server (e.g. vsftpd with
   `ssl_enable=YES`). Use the `PyftpdlibLiveTest` example as a
   starting point.

2. **Instrument `TLSSocketWrapper::close()`.** The source is in
   `cores/arduino/mbed/connectivity/netsocket/include/netsocket/TLSSocketWrapper.h`
   (header only; the cpp is in the precompiled `.a`). One option is to
   copy the cpp into the library's `src/` as a user override and add
   logging to find the exact blocking call.

3. **Try candidate 6.3** (per-file control reconnect). It is the
   lowest-risk path to real multi-file backup. If it works, the
   transport change becomes a policy decision rather than an
   engineering one.

4. **Try candidate 6.1** (bump `MEMP_NUM_NETCONN`). Quick confirmation
   of the socket-exhaustion hypothesis.

5. **Audit whether `TRANSPORT_KEEP` is actually being honoured.** If
   the destructor path in the linked `TLSSocketWrapper.cpp` calls
   `_transport->close()` regardless of `TRANSPORT_KEEP`, every
   attempt above is on sand.

---

## 7. How to reproduce

### 7.1 Host prerequisites

- Windows with `arduino-cli` ≥ 0.35 on PATH.
- mbed_opta core 4.5.0 (`arduino-cli core install arduino:mbed_opta`).
- Optional: `pyftpdlib` + `cryptography` for the local test server.

### 7.2 Minimal reproducer

Use `examples/PyftpdlibLiveTest`:

1. `cd examples/PyftpdlibLiveTest`
2. `python gen_cert.py` (creates `server.crt`/`server.key`)
3. `python ftps_server.py` (listens on 21/990)
4. Build and flash `PyftpdlibLiveTest.ino` to an Opta on COM3.
5. Over the terminal, watch the trace. Upload one file — succeeds.
   Attempt a second upload immediately — fails at `PASV`/`tcp-connect`.

### 7.3 Primary test harness used during this investigation

The `ArduinoSMSTankAlarm/TankAlarm-112025-Server-BluesOpta` sketch
orchestrates an HTTP `POST /api/ftp-backup` that walks several
config files. That is where the `filesUploaded: 1, filesFailed: N`
output originated. The library itself is reproducer-independent.

---

## 8. Glossary

- **`close_notify`** — TLS alert `21.0` signalling clean shutdown.
- **BIO** — mbedtls's abstract read/write callbacks. For
  `TLSSocketWrapper` these wrap `TCPSocket::send()`/`recv()`.
- **`TRANSPORT_KEEP`** — `TLSSocketWrapper` constructor flag that
  instructs the wrapper not to `close()` the underlying socket.
- **LWIP `netconn`** — Mbed's BSD-socket-like handle over LWIP; the
  actual scarce resource (`MEMP_NUM_NETCONN`, default 8).

---

## 9. References

- Mbed OS API header:
  `cores/arduino/mbed/connectivity/netsocket/include/netsocket/TLSSocketWrapper.h`
- mbedtls API header:
  `cores/arduino/mbed/connectivity/mbedtls/include/mbedtls/ssl.h`
- Library CHANGELOG entry: `CHANGELOG.md` → `[Unreleased]` →
  "FTPS backup hang / watchdog reset on Arduino Opta (Mbed OS)".
- RFC 4217 §9 (FTPS data-channel TLS ordering).

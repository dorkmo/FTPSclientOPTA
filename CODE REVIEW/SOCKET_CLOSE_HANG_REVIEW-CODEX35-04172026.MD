# ArduinoOPTA-FTPS Socket Close Hang Review (Codex 35)

**Date:** 2026-04-17  
**Reviewer:** GPT-5.3-Codex  
**Primary target:** `src/transport/MbedSecureSocketFtpsTransport.cpp`

---

## Scope and Method

I scanned the close-path and adjacent FTPS control/data lifecycle across:

- `ArduinoOPTA-FTPS/src/transport/MbedSecureSocketFtpsTransport.cpp`
- `ArduinoOPTA-FTPS/src/transport/MbedSecureSocketFtpsTransport.h`
- `ArduinoOPTA-FTPS/src/FtpsClient.cpp`
- `ArduinoOPTA-FTPS/src/FtpsClient.h`
- `ArduinoOPTA-FTPS/src/FtpsErrors.h`
- `ExternalAppRepo/ExternalServerSketch/ExternalServerSketch.ino`
- `ArduinoOPTA-FTPS/CHANGELOG.md`
- `ArduinoOPTA-FTPS/README.md`

This review focuses on defects and risks related to the socket-close hang, session teardown, and multi-file FTPS behavior.

---

## Executive Summary

The current workaround avoids watchdog resets, but it leaves multiple high-confidence defects/risk paths:

1. Data/control sockets are still intentionally leaked per transfer/session.
2. A blocking close path is still reachable in one control-channel failure branch.
3. TLS session-reuse plumbing exists but is not applied to data channel handshake.
4. A control-reply parsing behavior can convert hard disconnect into timeout stalls.
5. Integration-side diagnostics can mutate connection state and are currently ignored by caller logic.

The top two items are sufficient to explain the "one file uploads, second file fails" behavior and why reconnect-per-file diagnostics do not scale.

---

## Ranked Findings (By Confidence)

## 1) Intentional socket leak is still active in production close path
**Confidence:** 99%

**Evidence**
- `ftpsReleaseSocketPair()` sets pointers to null without deleting/closing resources:
  - `MbedSecureSocketFtpsTransport.cpp:98`
  - `MbedSecureSocketFtpsTransport.cpp:99`
- Data and control both use this path:
  - `MbedSecureSocketFtpsTransport.cpp:456`
  - `MbedSecureSocketFtpsTransport.cpp:469`

**Why this is a defect**
- This is the direct mechanism for LWIP `netconn` exhaustion during multi-file FTPS sessions.
- It also creates heap pressure over repeated backup operations.

**Recommended solutions**
- Short term (mitigation): increase LWIP socket pool (`MEMP_NUM_NETCONN`) in application config to raise failure threshold.
- Short term (behavioral): reconnect per file only as a temporary policy switch, with clear warning that it still leaks.
- Medium term: move close into a dedicated long-lived worker (single thread + queue), so the main thread can continue watchdog service while close blocks.
- Long term: patch Mbed close path (or bypass `TLSSocketWrapper` for teardown control).

---

## 2) Destructor and begin()/recreate path cannot reclaim leaked sockets
**Confidence:** 96%

**Evidence**
- Transport destructor does `closeAll(); destroySockets();`:
  - `MbedSecureSocketFtpsTransport.cpp:149`
  - `MbedSecureSocketFtpsTransport.cpp:150`
  - `MbedSecureSocketFtpsTransport.cpp:151`
- `closeAll()` routes into nulling close helper first, so `destroySockets()` sees null pointers.
- `FtpsClient::begin()` deletes and recreates transport repeatedly:
  - `FtpsClient.cpp:457`
  - `FtpsClient.cpp:458`
- Integration helper calls `begin()` on each FTPS connect flow:
  - `ExternalServerSketch.ino:5606`

**Why this is a defect**
- Recreating `FtpsClient` transport does not recover previously leaked sockets.
- Reconnect-per-file testing can amplify leak rate.

**Recommended solutions**
- Do not call `begin()` for every reconnect attempt; call once per process/network-init.
- Keep one transport instance alive for the entire backup operation.
- If leak mode must remain, document/implement an operational recycle policy (for example, reboot after backup job) until transport close is fixed.

---

## 3) Blocking close path is still reachable on control TLS fingerprint mismatch
**Confidence:** 93%

**Evidence**
- In `upgradeControlToTls()` mismatch path:
  - `_controlTls->close();` at `MbedSecureSocketFtpsTransport.cpp:382`
  - `_controlSocket->close();` at `MbedSecureSocketFtpsTransport.cpp:386`

**Why this is a defect**
- The exact class of close call known to hang is still used in this branch.
- Rare path, but high-impact when hit (can still stall watchdog path).

**Recommended solutions**
- Replace this branch with the same non-blocking teardown strategy (`ftpsReleaseSocketPair`) used elsewhere.
- Do not call direct TLS/TCP close in this branch on current Mbed core.

---

## 4) TLS session reuse helper exists but is not applied to data-channel handshake
**Confidence:** 90%

**Evidence**
- Helper exists: `applyCachedControlSession(...)` at `MbedSecureSocketFtpsTransport.cpp:186`.
- No call sites in current source.
- Data handshake currently goes straight to connect:
  - `_dataTls->connect()` at `MbedSecureSocketFtpsTransport.cpp:521`

**Why this is a defect/risk**
- Some FTPS servers require data-channel TLS session reuse with control session.
- Unused reuse hook increases risk of `SessionReuseRequired` / handshake failures on stricter servers.

**Recommended solutions**
- Call `applyCachedControlSession(*_dataTls)` immediately before `_dataTls->connect()`.
- If reuse apply fails, return explicit diagnostic error and optionally retry policy.

---

## 5) Control reply reader treats `recv()==0` as "no data yet" instead of EOF
**Confidence:** 86%

**Evidence**
- `ftpReadResponse()` path:
  - `if (readResult == 0) { ... continue; }` at `FtpsClient.cpp:269`
- Data read path treats `0` as end-of-stream in retrieve logic:
  - `FtpsClient.cpp:1278`

**Why this is a defect/risk**
- For many socket stacks, `recv()==0` means peer closed connection.
- Current behavior can convert immediate disconnect into timeout waits, causing avoidable stalls.

**Recommended solutions**
- Treat `readResult == 0` as connection closed in control channel logic (return failure immediately).
- If Mbed TLS occasionally emits transient zero, add a very small bounded grace counter instead of full timeout looping.

---

## 6) Integration diagnostic NOOP can alter session state but result is not enforced
**Confidence:** 82%

**Evidence**
- Post-store control probe:
  - `gFtpsClient.isControlAlive(...)` at `ExternalServerSketch.ino:5661`
- `ftpsStoreBuffer()` returns success even when probe fails.

**Why this is a defect/risk**
- `isControlAlive()` may disconnect/mark session dead on failures.
- Caller can treat upload as successful and continue with a now-dead session, causing confusing next-file failures.

**Recommended solutions**
- Gate this probe behind a dedicated diagnostic macro and disable in production path.
- If probe is enabled and fails, return failure to caller or force reconnect immediately.

---

## 7) Reconnect-per-file diagnostic mode is not a durable mitigation under leak mode
**Confidence:** 80%

**Evidence**
- Diagnostic switch default off:
  - `ExternalServerSketch.ino:219`
- Reconnect-per-file logic:
  - `ExternalServerSketch.ino:6225`
  - `ExternalServerSketch.ino:6273`

**Why this is a defect/risk**
- It can improve server-side state handling, but does not eliminate local socket leak behavior.
- Combined with repeated connect/quit cycles, it may leak faster under heavy backup workloads.

**Recommended solutions**
- Use reconnect-per-file only as a temporary operational fallback.
- Pair with socket-pool increase and explicit maintenance/restart strategy until close path is truly reclaiming sockets.

---

## 8) Documentation drift can mislead debugging and triage
**Confidence:** 98% (documentation issue)

**Evidence**
- README still states legacy close behavior as current:
  - `README.md:217`
  - `README.md:218`
- Changelog correctly describes abandon strategy and known limitation:
  - `CHANGELOG.md:22`
  - `CHANGELOG.md:34`
  - `CHANGELOG.md:35`

**Why this matters**
- Conflicting docs can cause incorrect expectations during incident response and regression triage.

**Recommended solutions**
- Update README "Current Status" and "Limitations" to match current transport behavior and known one-file limitation.

---

## 9) Future reuse work can accidentally reintroduce blocking close via `connectSocket()`
**Confidence:** 74%

**Evidence**
- `connectSocket()` reuse branch does `socket->close();`
  - `MbedSecureSocketFtpsTransport.cpp:232`

**Why this is a defect/risk**
- If the team implements socket preallocation/reuse (a likely next step), this branch can silently reintroduce the same close hang bug.

**Recommended solutions**
- Before enabling socket reuse, redesign this branch to avoid direct close on the active code path (or isolate close in worker thread).

---

## Recommended Fix Order

1. Eliminate remaining direct blocking-close branch in `upgradeControlToTls()` fingerprint mismatch path.
2. Wire control-session reuse into data TLS handshake (`applyCachedControlSession`).
3. Correct control read EOF handling (`recv()==0`).
4. Disable or harden integration NOOP probe side effects.
5. Decide and implement one explicit interim operating mode:
   - leak mode + larger socket table + operational recycle, or
   - worker-thread close path,
   while upstream Mbed close behavior is being addressed.
6. Align README with actual runtime behavior.

---

## Final Assessment

The current patch successfully prevents the original watchdog-reset hang in the common upload path, but the codebase still contains high-confidence defects that explain the remaining single-file limitation and several edge-path regressions. The most important code changes are localized and practical: remove the remaining direct close branch, apply session reuse hook, and fix EOF handling. The transport reclaim problem remains the strategic blocker and likely requires either Mbed patching or a robust asynchronous teardown architecture.
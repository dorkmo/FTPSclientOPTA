# ArduinoOPTA-FTPS Socket Close Hang Review (GPT-5.4)

**Date:** 2026-04-17  
**Scope:** Static code review of the current transport/client implementation related to the FTPS socket-close hang. This review is based on the current source tree and the existing handoff note in `CODE REVIEW/SOCKET_CLOSE_HANG_ANALYSIS_04172026.md`. I did not rerun hardware tests during this pass.

## Reviewed files

- `src/transport/MbedSecureSocketFtpsTransport.cpp`
- `src/transport/MbedSecureSocketFtpsTransport.h`
- `src/transport/IFtpsTransport.h`
- `src/FtpsClient.cpp`
- `CHANGELOG.md`
- `CODE REVIEW/SOCKET_CLOSE_HANG_ANALYSIS_04172026.md`

## Bottom line

The current workaround does remove the original long blocking close path, but it does so by eliminating real socket teardown. The most important code-level conclusion from this scan is that the leak is broader than the current handoff document suggests: the code now abandons both data-channel and control-channel sockets, not just one data socket per transfer.

I also found two remaining control-channel branches that still call the exact close/delete paths that the analysis says are unsafe. Those branches are narrower than the main success path, but they are still credible residual hang paths.

No compile-time/editor diagnostics were reported in the reviewed files. The problems below are runtime and lifecycle defects.

## Ranked findings

### 1. Very high confidence: `ftpsReleaseSocketPair()` is a pure-abandon helper, and it is used for both data and control channels

- **Evidence:** `src/transport/MbedSecureSocketFtpsTransport.cpp` lines 58-97 set non-blocking mode, send `mbedtls_ssl_close_notify()`, then null both pointers without calling `close()` or `delete`.
- **Evidence:** `closeData()` at line 456 and `closeControl()` at line 469 both call that helper.
- **Evidence:** `FtpsClient` calls `closeData()` after each upload/download (`src/FtpsClient.cpp` lines 1077 and 1294) and calls `closeAll()` on disconnect/error paths and on `quit()` (`src/FtpsClient.cpp` lines 439, 698, 809, 823, 840, 852, 864, 891, 900, 912, 1377).
- **Evidence:** the transport destructor (`src/transport/MbedSecureSocketFtpsTransport.cpp` line 149) calls `closeAll()` before `destroySockets()` (line 199), but `closeAll()` has already nulled the raw pointers, so the destructor cannot reclaim the leaked sockets afterward.

- **Why this matters:**
  The current implementation leaks `_dataSocket`, `_dataTls`, `_controlSocket`, and `_controlTls` objects, plus the underlying LWIP socket slots they own. The current handoff note and changelog describe the main symptom as one leaked data socket per transfer, but the code also leaks the control socket on every `quit()`, reconnect, and `failAndDisconnect()` path. That means "reconnect per file" is not a real workaround in the current codebase; it trades one failing pattern for another because each reconnect leaks another control socket.

- **Suggested solution:**
  Treat the current pure-abandon logic as a temporary diagnostic workaround, not as a shipping close strategy. A real fix has to reclaim the TCP/LWIP resource.

- **Most realistic fix paths:**
  1. Patch the Mbed close path so `TLSSocketWrapper::close()` / `TCPSocket::close()` honor timeout or non-blocking behavior.
  2. Bypass `TLSSocketWrapper` and own the `mbedtls_ssl_context` + BIO over `TCPSocket` directly so close ordering is under application control.
  3. Increase `MEMP_NUM_NETCONN` only as a diagnostic to confirm leak scaling, not as the final solution.

### 2. High confidence: the control-channel fingerprint-mismatch path still re-enters the blocking close APIs directly

- **Evidence:** in `src/transport/MbedSecureSocketFtpsTransport.cpp` lines 380-387, `upgradeControlToTls()` handles a failed `completePinnedFingerprintCheck()` by calling `_controlTls->close()`, `delete _controlTls`, and `_controlSocket->close()`.
- **Evidence:** the hang analysis explicitly states that `TLSSocketWrapper::close()`, `TCPSocket::close()`, and `delete tls` are the problematic paths on this platform/server combination.

- **Why this matters:**
  This is a residual hang path outside the normal success-path workaround. If the control TLS handshake succeeds but the fingerprint pin is wrong, the code goes straight back into the close path that originally caused the watchdog reset problem.

- **Suggested solution:**
  Replace this branch with the same non-blocking/abandon helper currently used elsewhere, or with a dedicated failure-teardown helper that never calls `_controlTls->close()` or `_controlSocket->close()` directly. Then test with an intentionally incorrect fingerprint to verify that the client fails cleanly instead of hanging.

### 3. Medium-high confidence: the control TLS handshake-failure path still deletes `TLSSocketWrapper` directly after `connect()` returns an error

- **Evidence:** in `src/transport/MbedSecureSocketFtpsTransport.cpp` lines 371-375, `upgradeControlToTls()` does:

```cpp
_lastTlsError = _controlTls->connect();
if (_lastTlsError != NSAPI_ERROR_OK) {
    delete _controlTls;
    _controlTls = nullptr;
    return failWith(error, errorSize, "TLS handshake on the control channel failed.");
}
```

- **Why this matters:**
  The analysis document already notes that `delete tls` re-enters Mbed's internal close path. The normal success-path bug is easy to reproduce, but this failure branch is still risky too: auth failures, cert failures, or mid-handshake aborts may leave the wrapper in a state where its destructor still tries to close underlying transport state synchronously.

- **Suggested solution:**
  After any call to `_controlTls->connect()`, route cleanup through one teardown strategy only. Do not `delete _controlTls` directly on post-connect failure. If the current workaround must remain in place, use the same helper used by `closeControl()` so behavior is at least consistent.

### 4. High confidence: the cached control TLS session is never applied anywhere, so the advertised "best-effort session reuse" is currently dead code

- **Evidence:** `cacheControlSession()` is implemented at `src/transport/MbedSecureSocketFtpsTransport.cpp` line 170 and called after control-channel TLS success at line 378.
- **Evidence:** `applyCachedControlSession()` exists at line 186.
- **Evidence:** there are no call sites for `applyCachedControlSession()` anywhere in `src/`.
- **Evidence:** `upgradeDataToTls()` (starts at line 497) creates `_dataTls` and calls `_dataTls->connect()` directly without trying to seed the cached control session.

- **Why this matters:**
  This is not the primary socket-close bug, but it affects the review of potential fixes. Earlier repo documents describe session reuse as partially implemented. The current code does not actually apply the cached session anywhere. That means:
  - servers requiring reuse can still fail exactly as if no reuse support existed;
  - any proposal that depends on "cached session tickets make reconnect-per-file cheap" is weaker than the docs imply;
  - the repository documentation currently overstates the state of the implementation.

- **Suggested solution:**
  If session reuse remains a goal, call `applyCachedControlSession(*_dataTls)` before `_dataTls->connect()` in `upgradeDataToTls()`. If reconnect-per-file becomes the policy, also revisit whether `closeControl()` should clear the cached control session immediately or preserve it long enough for the next TLS handshake.

### 5. Medium confidence: `connectSocket()` still contains synchronous `socket->close()` paths that will likely reintroduce the same hang if socket reuse is attempted as a fix

- **Evidence:** `src/transport/MbedSecureSocketFtpsTransport.cpp` lines 211-245 call `socket->close()` in the reuse path and again in the TCP-connect failure path.
- **Evidence:** the handoff note lists "pre-allocate and reuse a single `TCPSocket`" as one of the candidate fixes.

- **Why this matters:**
  Right now this code is mostly masked because the current close helper nulls the socket pointers, so `connectSocket()` usually allocates a new `TCPSocket`. But if the next fix attempt reintroduces socket reuse, this helper still closes reused sockets synchronously. That creates a real risk of walking straight back into the same Mbed close-path bug under a different name.

- **Suggested solution:**
  If you pursue socket reuse, do not build it on top of the current `connectSocket()` implementation unchanged. Split "allocate/open/connect" from "release/recycle" and make sure the recycle path is compatible with whatever close workaround is actually chosen.

### 6. Medium confidence: comments and changelog text no longer match the implementation, which makes the close-path state easier to misunderstand than it should be

- **Evidence:** `src/transport/MbedSecureSocketFtpsTransport.cpp` line 48 says the current approach is to "delete both objects".
- **Evidence:** the same file at lines 83-94 says the safe path is pure abandon.
- **Evidence:** `closeData()` comment at line 461 says the code deletes heap objects so the socket handle is returned to Mbed.
- **Evidence:** the changelog at `CHANGELOG.md` lines 22-44 correctly describes pure abandon, but line 34 frames the known limitation as one LWIP handle per transfer, which undercounts the control-channel leak.

- **Why this matters:**
  This is a maintenance defect, not the root runtime bug, but it increases the chance of making the next fix attempt worse. A future reviewer can easily conclude that handles are reclaimed when they are not, or conclude that reconnect-per-file is a valid mitigation when the control side is also leaking.

- **Suggested solution:**
  Update the comments and release notes to state the exact current behavior:
  - both data and control close paths currently abandon resources;
  - the destructor does not recover those resources later;
  - reconnect-per-file is not a complete workaround while control-close still leaks.

## Recommended order of work

1. **Remove the residual direct control close/delete branches first.**
   The fingerprint-mismatch and post-`connect()` failure paths are the narrowest remaining ways to re-enter the original hang.

2. **Correct the repo documentation and comments immediately after that.**
   The code and the current prose are no longer aligned closely enough.

3. **Choose a real resource-reclaim strategy.**
   My recommendation order is:
   - patch Mbed close behavior if you can carry a local core patch safely;
   - otherwise replace `TLSSocketWrapper` with explicit Mbed TLS over `TCPSocket` so close ordering becomes deterministic;
   - do not treat `MEMP_NUM_NETCONN` tuning as anything more than a confirmation tool.

4. **Do not rely on reconnect-per-file until control teardown is fixed.**
   In the current implementation, reconnecting per file just leaks control sockets faster.

5. **If session reuse still matters, wire it for real and retest.**
   Right now the cache exists, but the data channel never consumes it.

## Final assessment

The existing analysis correctly identified the original blocking-close problem, but the current code scan shows that the transport is now in a "survive the close call by not closing" state. That is a valid temporary field workaround for a watchdog-reset bug, but it is not yet a stable transport lifecycle.

The highest-confidence engineering conclusion is this: the library currently has no real socket release path, and the leak affects both channels. Until that is fixed, multi-file transfer and reconnect-based recovery will remain structurally unreliable even if the original long hang is gone.
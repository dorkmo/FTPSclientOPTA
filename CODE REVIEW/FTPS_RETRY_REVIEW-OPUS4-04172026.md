# FTPS File Transfer Retry — Review & Recommendations (Opus 4.6)

**Date:** 2026-04-17  
**Scope:** Cross-repository review of FTPS retry proposals for ArduinoOPTA-FTPS and ExternalAppRepo.  
**Documents reviewed:**
- `ArduinoOPTA-FTPS/CODE REVIEW/FTPS_RETRY_PROPOSALS_SUMMARY_04172026.md`
- `ArduinoOPTA-FTPS/CODE REVIEW/SOCKET_CLOSE_HANG_ANALYSIS_04172026.md`
- `ArduinoOPTA-FTPS/CODE REVIEW/SOCKET_CLOSE_HANG_REVIEW-GPT54-04172026.MD`
- `ArduinoOPTA-FTPS/CODE REVIEW/SOCKET_CLOSE_HANG_REVIEW-CODEX35-04172026.MD`
- `ArduinoOPTA-FTPS/CODE REVIEW/SOCKET_CLOSE_HANG_REVIEW-GEMINI31-04172026.MD`
- `ExternalAppRepo/CODE REVIEW/PER_FILE_RETRY_PLAN_04172026.md`
- `ExternalAppRepo/CODE REVIEW/MULTI_FILE_BACKUP_FOLLOWUPS_04172026.md`
- `ExternalAppRepo/CODE REVIEW/OPTA_LWIP_BACKUP_RECIPE_04172026.md`
- `ExternalAppRepo/CODE REVIEW/NIGHTLY_BACKUP_PLAN_04172026.md`
- Current source: `FtpsClient.cpp`, `MbedSecureSocketFtpsTransport.cpp`, `ExternalServerSketch.ino`

---

## Executive Summary

The existing proposals are well-structured and mostly sound. The per-file retry plan is the right next step. However, several gaps exist between the proposals and the current code that should be addressed before implementation. This review identifies those gaps and offers concrete improvements.

The most important findings:

1. The retry proposals were written partly against the old abandon-only close path. The `SO_LINGER` fix has since landed and `proc=8 failed=0` is verified. Retry logic is still valuable as a safety net but the urgency framing needs updating.
2. The `isControlAlive()` NOOP probe after every STOR is counterproductive — it mutates session state and can cause false negatives that poison retry decisions.
3. The per-file retry plan lacks an exponential or adaptive backoff component and does not account for the case where retry itself accelerates TIME_WAIT accumulation.
4. The single-STOR bundle proposal is underspecified for the restore path and should be designed before being committed to.
5. The library should expose more diagnostic state to make application-layer retry decisions precise.

---

## Part 1: Assessment of Existing Proposals

### 1.1 Per-File Retry Plan — Strong Foundation, Needs Refinement

**What's good:**
- Error classification (`DataConnectionFailed` vs. control-channel failures) is correct.
- Bounded attempts (3 max) are appropriate for the constrained environment.
- "Continue to next file on exhausted retries" is the right graceful-degradation strategy.
- The `isDataPoolFailure()` helper concept is clean.
- Test plan is thorough.

**What needs work:**

#### A. Fixed 30s retry wait is too aggressive in some cases and wasteful in others

The plan proposes a flat 30,000ms wait between retries. But the TIME_WAIT drain time is not constant — it depends on when the previous socket entered TIME_WAIT relative to the retry attempt.

**Improvement:** Use a stepped backoff: 15s on the first retry, 35s on the second retry. The first retry catches cases where the socket was already mid-drain from the inter-file wait. The second retry catches genuine full TIME_WAIT cycles. Total worst-case per file is 50s of retry wait instead of 60s, but the common (first-retry-succeeds) case saves 15s.

```cpp
static const uint32_t kRetryWaitMs[] = { 15000UL, 35000UL };
// Index: attempt - 1 (0-based), capped at array size - 1
```

#### B. Retry itself creates a new TIME_WAIT entry

Each failed `store()` attempt opens a data socket, fails to connect or fails mid-transfer, and then closes that socket — adding another entry to the TIME_WAIT pool. The plan does not account for this. If the pool has exactly 0 free slots and a retry opens+fails a socket, the retry has made the situation worse: now two sockets are in TIME_WAIT instead of one.

**Improvement:** Before retrying, check whether the library's last failure occurred *before* a data socket was opened vs. *after*. The library already traces `xport:open-failed:-3005` (socket allocation failed — no socket was consumed) vs. `xport:connect-failed:-3005` (socket was opened but connect failed — a socket WAS consumed). Only the `open-failed` case is safe for immediate retry. If the failure was `connect-failed` or later, the retry wait should be at least 60s (a full TIME_WAIT cycle).

**Library enhancement needed:** Expose whether the data socket was opened before the failure. A simple approach:

```cpp
// In FtpsClient.h — new public method
bool lastFailureConsumedDataSocket() const;
```

Or alternatively, expose the phase at failure (already partially available via `lastPhase()`).

#### C. Missing guard against retry during control-channel degradation

The plan correctly says "don't retry if control channel is dead." But `ftpsSessionLikelyDead()` only checks the last `FtpsError` enum. It does not detect the case where the control channel is *degraded* (responding slowly, or NOOP works but PASV will fail). A file that fails on `DataConnectionFailed` three times in a row, where each failure occurs further into the STOR process, may indicate a server-side problem rather than LWIP pool exhaustion.

**Improvement:** If all retry attempts fail with `DataConnectionFailed` for the same file, increment a "consecutive data failures" counter across files. If that counter hits a configurable threshold (e.g. 3 consecutive files each exhausting all retries), abort the batch. This prevents burning 3 × 50s = 150s of retry time on every remaining file when the underlying problem is systemic.

```cpp
static uint8_t consecutiveDataFailFiles = 0;
// Reset to 0 on any successful store
// Increment when a file exhausts all retry attempts with DataConnectionFailed
// Abort batch if >= FTP_BACKUP_MAX_CONSECUTIVE_FAIL_FILES (default 2)
```

### 1.2 Inter-File Delay Tuning — Agree With 45s Target

The follow-up document's recommendation to reduce from 65s to 45s once retry exists is sound. The 65s value includes a 5s margin over 2×MSL; 45s still provides 15s of margin for typical LWIP implementations where TIME_WAIT often completes in 30–35s.

**One addition:** Make the inter-file delay a named constant and log the actual elapsed time of the data-socket-open attempt. If the open succeeds on the first try, the delay was sufficient. If it requires a retry, the delay was too short. This data, logged over several backup cycles, will empirically calibrate the delay.

```cpp
#ifndef FTP_BACKUP_INTER_FILE_DELAY_MS
#define FTP_BACKUP_INTER_FILE_DELAY_MS 45000UL
#endif
```

### 1.3 Single-STOR Bundle — Promising but Underspecified

Bundling all files into a single upload is the most impactful throughput improvement (11 minutes → ~10 seconds). However:

**Problem 1: Restore path.** The existing restore loop calls `ftpsRetrieveBuffer()` per file and writes each to `/fs/filename`. A bundled upload requires either:
- A server-side unpacking step (breaks the "FTPS server is a generic FTP server" assumption), or
- A custom envelope format that the Opta can also parse on retrieve.

**Problem 2: Atomic failure.** If a single-STOR upload of a 16KB bundle fails at byte 12,000, all 8 files are lost. Per-file uploads guarantee that files uploaded before the failure are safe on the server.

**Problem 3: RAM.** The current per-file buffer is 2KB. A bundle of 8 files at up to 2KB each is 16KB plus envelope overhead. The Opta has ~512KB SRAM, so this is feasible, but the buffer must be heap-allocated and checked.

**Recommendation:** If bundling is pursued, use a simple JSON envelope rather than tar/zip (no decompression library needed on either end). Design the restore parser first and verify it round-trips correctly before building the upload path. An incremental alternative: upload a manifest file listing all backup files, then upload the individual files. If a future version wants to bundle, the manifest already exists.

### 1.4 Nightly Backup Plan — Clean Design, One Gap

The nightly backup plan correctly reuses the daily-email scheduler pattern. One gap:

**Gap: No backup-failure notification.** The plan defers failure notification to "Open Questions." For an unattended nightly backup, silent failure is the worst outcome. At minimum, the nightly backup should set a persistent flag (e.g. `gLastNightlyBackupFailed = true`) that the web dashboard displays as a warning banner. This costs ~5 lines and ensures an operator sees the failure within 24 hours.

### 1.5 Reconnect-Per-File — Correctly Rejected for Now

All reviewers agree that reconnect-per-file under the old abandon-only close path leaks control sockets and accelerates pool exhaustion. With the SO_LINGER fix now landed, reconnect-per-file is theoretically viable again, but it adds ~1.5s per file (full TLS handshake) and the TIME_WAIT problem still exists for both data and control sockets.

**Recommendation:** Keep reconnect-per-file disabled by default. It should remain as a diagnostic escape hatch (`FTP_BACKUP_FTPS_RECONNECT_PER_FILE_DIAG`) for debugging server-side issues, not as a production strategy.

---

## Part 2: Code-Level Issues Affecting Retry

### 2.1 Remove the Post-STOR `isControlAlive()` Probe

**File:** `ExternalServerSketch.ino`, `ftpsStoreBuffer()` (~line 5690)

The current code calls `gFtpsClient.isControlAlive()` after every successful STOR. This sends a NOOP command to the server. Problems:

1. **It mutates the FTP session state.** If the server is slow to respond to NOOP after a heavy STOR, the 15s reply timeout may fire, causing `failAndDisconnect()`. The next file's store attempt then sees a dead session — not because the session was actually dead, but because the NOOP probe killed it.

2. **It confuses retry decisions.** If the NOOP fails and marks the session dead, the retry logic (once implemented) will see `ControlIoFailed` from the NOOP and abort the batch — even though a simple retry of the STOR (without the intermediate NOOP) might have succeeded.

3. **It was added as a diagnostic.** The comment says "Diagnostic: Check if control connection is still alive." Now that multi-file backup is verified working (`proc=8 failed=0`), this diagnostic probe should be removed or guarded behind a compile-time flag.

**Recommendation:** Remove the `isControlAlive()` call from `ftpsStoreBuffer()`. If control-channel health monitoring is still desired, move it to the inter-file wait period (after the TIME_WAIT drain, before the next STOR), where a failed NOOP can trigger a reconnect instead of polluting the STOR result.

### 2.2 Library Should Expose Richer Failure Context

The current `FtpsError lastError()` API returns a single enum. For application-level retry decisions, the integrator needs:

| Information | Current availability | Proposed |
|-------------|---------------------|----------|
| Error category (data vs. control) | Partially — inferred from enum value | No change needed |
| NSAPI error code | Not exposed | Add `int lastNsapiError() const` |
| Phase at failure | `lastPhase()` returns a string | Sufficient for logging, but not for programmatic branching |
| Data socket was consumed | Not exposed | Add `bool lastFailureOpenedDataSocket() const` |

The NSAPI code is important because `DataConnectionFailed` is currently overloaded: it covers `-3005` (pool exhaustion, transient), `-3008` (connection timeout, likely persistent), and `-3001` (DNS failure, permanent). The retry plan treats all three identically, but only `-3005` is genuinely retriable.

**Minimum viable change:** Add `int lastNsapiError() const` to `FtpsClient`. The transport already records `_lastTcpError` and `_lastTlsError`; expose whichever is most recent through the client.

### 2.3 The `ftpsSessionLikelyDead()` Helper Needs a Rethink

The current implementation in the server sketch checks for a fixed set of `FtpsError` values:

```
ConnectionFailed, PassiveModeRejected, DataConnectionFailed, FinalReplyFailed
```

With the retry plan, `DataConnectionFailed` should be *excluded* from the "session likely dead" set (since it's now retriable). But the current plan proposes checking both:

```
if ftpsSessionLikelyDead(...) and not isDataPoolFailure(...):
    abortRemainingTransfers = true
```

This works but is fragile — `ftpsSessionLikelyDead()` returns true for `DataConnectionFailed`, then the caller immediately overrides that with `isDataPoolFailure()`. Cleaner approach:

**Recommendation:** Split into two distinct helpers:

```cpp
// Session is dead — control channel gone, no point continuing
static bool ftpsSessionDead(FtpsError err) {
  return err == FtpsError::ConnectionFailed ||
         err == FtpsError::ControlIoFailed;
}

// Transfer failed but session may survive — data-channel transient
static bool ftpsTransferRetriable(FtpsError err) {
  return err == FtpsError::DataConnectionFailed;
}
```

The backup loop then becomes:
```
if ftpsSessionDead(lastError): abort batch
else if ftpsTransferRetriable(lastError): retry this file
else: mark failed, continue to next file
```

This eliminates the double-check pattern and makes the retry/abort logic self-documenting.

### 2.4 The 2KB File Buffer Silently Truncates Large Files

`performFtpBackupDetailed()` reads each file into a 2KB stack buffer (`char contents[2048]`), but `FTP_MAX_FILE_BYTES` is set to 24KB. If a config file grows beyond 2KB (which client config manifests could approach with many clients), the upload silently truncates. The restore then loads a corrupted file.

**Recommendation:** Either:
- Increase the buffer to match `FTP_MAX_FILE_BYTES` (heap-allocate if stack is too tight), or
- Check `len == sizeof(contents)` after the read and log a warning/skip the file, or
- Use the library's streaming upload if/when it's added.

This isn't directly a retry issue, but a retry loop that succeeds on a truncated file is worse than a failure.

---

## Part 3: Recommended Implementation Order

### Phase 1 — Immediate (before implementing retry)

1. **Remove `isControlAlive()` from `ftpsStoreBuffer()`.** This is a pure diagnostic that can interfere with production transfers.
2. **Add `int lastNsapiError() const` to `FtpsClient`.** One getter, no API breakage. Enables precise retry gating.
3. **Refactor `ftpsSessionLikelyDead()` into `ftpsSessionDead()` + `ftpsTransferRetriable()`.** Cleaner foundation for retry logic.

### Phase 2 — Per-File Retry

4. **Implement the retry loop** per `PER_FILE_RETRY_PLAN_04172026.md`, with these modifications:
   - Stepped backoff (15s / 35s) instead of flat 30s.
   - Gate on `lastNsapiError() == -3005` for precise pool-exhaustion detection.
   - Add a consecutive-failure-files counter with an abort threshold.
5. **Reduce inter-file delay to 45s.** Safe with retry as a backstop.
6. **Extract the inter-file delay to a `#define`.** Enables runtime tuning without recompile.

### Phase 3 — Resilience

7. **Add nightly backup** per `NIGHTLY_BACKUP_PLAN_04172026.md`, with a persistent failure flag for the dashboard.
8. **Add a truncation guard** for the 2KB buffer in `performFtpBackupDetailed()`.

### Phase 4 — Throughput (Optional)

9. **Design the bundle/envelope format** (JSON array with base64-encoded file contents) and implement a round-trip test before building the upload path.
10. **Reduce inter-file delay further** based on empirical data from Phase 2 logs.

---

## Part 4: Proposed Retry Code Sketch

This is a minimal diff sketch for `performFtpBackupDetailed()`. It incorporates the stepped backoff, NSAPI-level gating, and consecutive-failure tracking.

```cpp
// --- New constants (near top of file) ---
#ifndef FTP_BACKUP_PER_FILE_MAX_ATTEMPTS
#define FTP_BACKUP_PER_FILE_MAX_ATTEMPTS  3
#endif
#ifndef FTP_BACKUP_INTER_FILE_DELAY_MS
#define FTP_BACKUP_INTER_FILE_DELAY_MS    45000UL
#endif
#ifndef FTP_BACKUP_MAX_CONSECUTIVE_FAIL_FILES
#define FTP_BACKUP_MAX_CONSECUTIVE_FAIL_FILES  2
#endif

static const uint32_t kRetryBackoffMs[] = { 15000UL, 35000UL };

// --- New helpers ---
static bool ftpsSessionDead(FtpsError err) {
  return err == FtpsError::ConnectionFailed ||
         err == FtpsError::ControlIoFailed;
}

static bool ftpsTransferRetriable(FtpsError err, int nsapiCode) {
  // Only retry on LWIP pool exhaustion specifically
  return err == FtpsError::DataConnectionFailed && nsapiCode == -3005;
}

// --- Inside the per-file loop in performFtpBackupDetailed() ---
//     Replace the single ftpsStoreBuffer() call with:

uint8_t consecutiveFailFiles = 0;

for (size_t i = 0; i < fileCount; ++i) {
  if (abortRemainingTransfers) break;

  // Inter-file delay (unchanged except for the constant)
  if (i > 0 && useFtps) {
    Serial.println(F("FTPS phase: inter-file:wait-tw"));
    uint32_t waitStart = millis();
    while (millis() - waitStart < FTP_BACKUP_INTER_FILE_DELAY_MS) {
      serviceTransferWatchdog();
      delay(100);
    }
    Serial.println(F("FTPS phase: inter-file:wait-done"));
  }

  // ... read file into buffer (unchanged) ...

  bool stored = false;
  uint8_t attempts = 0;

  while (attempts < FTP_BACKUP_PER_FILE_MAX_ATTEMPTS) {
    attempts++;
    stored = ftpsStoreBuffer(remotePath, (const uint8_t *)contents, len,
                             err, sizeof(err));
    if (stored) break;

    FtpsError lastErr = gFtpsClient.lastError();
    int lastNsapi = gFtpsClient.lastNsapiError();

    // Hard failure — abort immediately
    if (ftpsSessionDead(lastErr)) break;

    // Non-retriable data failure — skip file, don't retry
    if (!ftpsTransferRetriable(lastErr, lastNsapi)) break;

    // Retriable — wait and try again
    if (attempts < FTP_BACKUP_PER_FILE_MAX_ATTEMPTS) {
      uint8_t backoffIdx = min((uint8_t)(attempts - 1),
                               (uint8_t)(sizeof(kRetryBackoffMs)/sizeof(kRetryBackoffMs[0]) - 1));
      uint32_t waitMs = kRetryBackoffMs[backoffIdx];
      Serial.printf("FTP retry: %s attempt %u/%u, wait %lu ms\r\n",
                    entry.remoteName, attempts + 1,
                    FTP_BACKUP_PER_FILE_MAX_ATTEMPTS, waitMs);
      uint32_t retryStart = millis();
      while (millis() - retryStart < waitMs) {
        serviceTransferWatchdog();
        delay(100);
      }
    }
  }

  if (stored) {
    result.filesProcessed++;
    consecutiveFailFiles = 0;  // Reset on success
  } else {
    result.filesFailed++;
    result.addFailedFile(entry.remoteName);

    FtpsError lastErr = gFtpsClient.lastError();
    if (ftpsSessionDead(lastErr)) {
      abortRemainingTransfers = true;
      strlcpy(result.errorMessage,
              "FTPS session dropped; aborted remaining uploads",
              sizeof(result.errorMessage));
    } else if (ftpsTransferRetriable(lastErr, gFtpsClient.lastNsapiError())) {
      consecutiveFailFiles++;
      if (consecutiveFailFiles >= FTP_BACKUP_MAX_CONSECUTIVE_FAIL_FILES) {
        abortRemainingTransfers = true;
        strlcpy(result.errorMessage,
                "Too many consecutive data failures; aborting backup",
                sizeof(result.errorMessage));
      }
      // Otherwise: file failed but keep trying remaining files
    }
  }
}
```

---

## Part 5: Summary of Differences from Existing Proposals

| Topic | Existing proposal | This review's recommendation |
|-------|-------------------|------------------------------|
| Retry backoff | Flat 30s | Stepped: 15s then 35s |
| Retry gating | `DataConnectionFailed` enum only | `DataConnectionFailed` + `lastNsapiError == -3005` |
| Batch abort on repeated failure | Not addressed | Abort after 2 consecutive files exhaust retries |
| `isControlAlive()` after STOR | Active in production | Remove — it can cause false session-dead signals |
| Session-dead check | Single `ftpsSessionLikelyDead()` | Split into `ftpsSessionDead()` + `ftpsTransferRetriable()` |
| Inter-file delay | 65s, reduce to 45s later | 45s immediately (with retry as safety net) |
| Bundle/single-STOR | Listed as long-term optimization | Design restore path first; use JSON envelope, not tar |
| Nightly backup failure notification | Deferred | Add persistent dashboard flag at minimum |
| 2KB buffer truncation | Not mentioned | Add truncation guard or increase buffer |
| Library NSAPI error exposure | Not proposed | Add `lastNsapiError()` to `FtpsClient` |

---

## Conclusion

The per-file retry plan is the correct next step. The modifications proposed here — stepped backoff, NSAPI-level gating, consecutive-failure abort, and removing the `isControlAlive()` probe — make the retry logic more precise and less likely to cause secondary failures. The library-side change (`lastNsapiError()`) is small but enables significantly better application-level decision-making.

The 65s → 45s delay reduction combined with retry brings estimated backup time from ~656s to ~430s for 8 files in the common no-failure case, and handles transient pool exhaustion gracefully rather than aborting the entire batch.

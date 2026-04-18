# FTPS File Transfer Retry Review (GPT-5.4)

**Date:** 2026-04-17  
**Repositories reviewed:** `ExternalAppRepo`, `ArduinoOPTA-FTPS`  
**Focus:** Review the existing FTPS retry proposals and recommend safer retry behavior for multi-file backup/restore flows on Arduino Opta.

## Bottom line

The current External App per-file retry proposal is worth doing, but it should be tightened before implementation.

The main improvement is this: do **not** treat every `FtpsError::DataConnectionFailed` as "retryable socket pool exhaustion." In the current FTPS library, that error is still too broad. The application should retry only when the failure is specifically the Opta/LWIP `-3005` no-socket condition, and it should apply the same retry/pacing wrapper to **all** FTPS uploads in the batch, including `clients_manifest.txt` and per-client snapshot files.

The second important change is to stop using the post-`STOR` `NOOP` probe as part of normal backup behavior. That probe can disconnect the control session on its own failure and interfere with the retry logic.

## Reviewed inputs

- `ExternalAppRepo/CODE REVIEW/PER_FILE_RETRY_PLAN_04172026.md`
- `ExternalAppRepo/CODE REVIEW/MULTI_FILE_BACKUP_FOLLOWUPS_04172026.md`
- `ExternalAppRepo/CODE REVIEW/OPTA_LWIP_BACKUP_RECIPE_04172026.md`
- `ExternalAppRepo/ExternalServerSketch/ExternalServerSketch.ino`
- `ArduinoOPTA-FTPS/src/FtpsClient.cpp`
- `ArduinoOPTA-FTPS/src/FtpsClient.h`
- `ArduinoOPTA-FTPS/src/FtpsErrors.h`
- `ArduinoOPTA-FTPS/src/transport/MbedSecureSocketFtpsTransport.cpp`
- `ArduinoOPTA-FTPS/CHANGELOG.md`

## Findings

### 1. `DataConnectionFailed` is not precise enough as the only retry trigger

The current proposal suggests treating `FtpsError::DataConnectionFailed` as the retryable bucket. That is directionally correct, but still too coarse for production use.

In the current library, `FtpsClient::store()` returns `DataConnectionFailed` when `openDataChannel()` fails before `STOR` data transfer begins. That includes the expected Opta `-3005` socket-pool exhaustion case, but it can also include other TCP open/connect failures that will not improve after a 30-second sleep.

**Recommendation:** for v1, retry only when all of the following are true:

- `gFtpsClient.lastError() == FtpsError::DataConnectionFailed`
- `gFtpsClient.lastPhase()` is still in the pre-write path, typically `store:data-open`
- the error text or trace proves the underlying `nsapi` failure was `-3005`

The current code already gives the application enough information for a first cut:

- transport error text includes `Failed to open TCPSocket (nsapi=%d)` or `TCP connect failed (nsapi=%d)`
- transport trace emits `xport:open-failed:<nsapi>` and `xport:connect-failed:<nsapi>`

That means the application can implement a conservative stopgap now by matching `nsapi=-3005` in the error text, without waiting for a library API change.

### 2. The retry proposal should not reuse `ftpsSessionLikelyDead()` unchanged

`ftpsSessionLikelyDead()` currently returns `true` for all of these:

- `ConnectionFailed`
- `PassiveModeRejected`
- `DataConnectionFailed`
- `FinalReplyFailed`

That helper is too broad for retry policy. It is an abort-oriented classifier, not a retry-oriented classifier.

If the retry plan is added without splitting these concepts, the code will keep fighting itself:

- retry logic will want to keep going on `DataConnectionFailed` with `-3005`
- the existing abort helper will still label the same failure as session-dead

**Recommendation:** introduce two separate helpers instead of one shared predicate:

- `shouldRetryFtpsStoreFailure(...)`
- `shouldAbortRemainingFtpsUploads(...)`

For v1, `shouldRetryFtpsStoreFailure(...)` should be intentionally narrow and only cover proven `-3005` pre-write failures.

### 3. The post-`STOR` control probe should not run in the normal retry path

`ftpsStoreBuffer()` currently calls `gFtpsClient.isControlAlive()` after every successful `store()` and logs either `ftps-ctrl:ok` or `ftps-ctrl:dead`.

That looks harmless, but `isControlAlive()` is not a passive read-only check. It sends `NOOP`, and if that command fails it calls `failAndDisconnect(...)`, which closes the FTPS session.

This creates two problems:

- it mutates session state after a successful upload
- it can convert a recoverable next-transfer failure into a forced disconnect caused by the diagnostic probe itself

**Recommendation:** remove this probe from the default backup path, or gate it behind a dedicated debug macro that stays off in shipping retry logic.

### 4. The current proposal does not cover the client-config upload path

The retry plan is scoped to the `performFtpBackupDetailed()` loop over `kBackupFiles`. That is not enough.

`ftpBackupClientConfigs()` uploads:

- `clients_manifest.txt`
- one `clients/<uid>.json` file per cached client config

but it does so with:

- no 65-second inter-file pacing
- no bounded retry loop
- no per-file failure accumulation for snapshot uploads

In practice, that means the main backup files may be protected while the follow-on client snapshot uploads still fail under the exact same Opta/LWIP constraints.

**Recommendation:** build a reusable helper such as `ftpsStoreBufferWithRetry(...)` and call it from both:

- `performFtpBackupDetailed()`
- `ftpBackupClientConfigs()`

Also record failed client snapshot files in `FtpResult.failedFiles` instead of silently skipping them.

### 5. Add verify-before-reupload for ambiguous post-write failures

Not every failed `STOR` should be retried the same way.

Once the payload has already been written and the code is waiting for the final `226` or `250`, a failure becomes ambiguous: the server may already have committed the file even if the control reply is lost.

The current library exposes enough phase information to treat that as a distinct class:

- `store:final-reply-read`
- `store:final-reply-failed`
- `store:final-reply-bad-code`

Blindly re-uploading after those phases risks duplicate or unnecessary transfers.

**Recommendation:** for `FinalReplyFailed` or `ConnectionFailed` in the final-reply path:

1. reconnect the control session if needed
2. call `SIZE` on the target path
3. if the remote size matches the expected payload length, treat the upload as success
4. only retry the upload if the file is missing or clearly short

There is already a version of this pattern in `performFtpBackupDetailed()` for the reconnect-per-file diagnostic flow. Promote that idea into the normal failure-handling path, but only for these ambiguous post-write failures.

### 6. The library should expose structured retry metadata

Today the application has to infer retryability from a mix of:

- `FtpsError`
- `lastPhase()`
- human-readable error text
- optional transport trace strings

That works for a first application-side patch, but it is brittle.

**Recommendation:** add one of the following library-level follow-ups in `ArduinoOPTA-FTPS`:

- `int lastNsapiError() const`
- `const char *lastTransportPhase() const`
- `FtpsRetryHint lastRetryHint() const`

The minimal useful addition is `lastNsapiError()`. That would let External App distinguish `-3005` from every other `DataConnectionFailed` case without parsing strings.

## Recommended v1 retry policy

Use a deliberately conservative policy first.

| Failure signal | Recommended action | Reason |
| --- | --- | --- |
| `DataConnectionFailed` + `nsapi=-3005` + pre-write phase | Wait 30 s, retry same file in same session, max 2 retries | This is the known Opta/LWIP exhaustion case |
| `FinalReplyFailed` or `ConnectionFailed` in `store:final-reply-*` | Reconnect if needed, verify with `SIZE`, retry only if size mismatches | Upload may already have succeeded |
| `PassiveModeRejected`, login/TLS/cert failures, bad fingerprint, `SessionReuseRequired` | Abort batch, no retry | These are configuration or server-policy failures |
| Any success path | Do not send `NOOP` automatically after `STOR` | Avoid state mutation from diagnostics |

## Suggested implementation shape

### Application side (`ExternalAppRepo`)

Add a reusable helper around FTPS uploads rather than inlining retry logic in one loop only.

Suggested shape:

```cpp
enum class FtpsStoreDecision {
  Success,
  RetrySameSession,
  VerifyThenMaybeRetry,
  AbortBatch,
  FailThisFile
};

static FtpsStoreDecision classifyFtpsStoreFailure(
    FtpsError error,
    const char *phase,
    const char *errorText);

static bool ftpsStoreBufferWithRetry(
    const char *remoteFile,
    const uint8_t *data,
    size_t len,
    char *error,
    size_t errorSize,
    bool &abortBatch);
```

Then use that helper in:

- the main `kBackupFiles` upload loop
- `ftpBackupClientConfigs()`

This keeps the retry policy consistent across all FTPS uploads.

### Library side (`ArduinoOPTA-FTPS`)

Keep the current transport behavior unchanged for the first application retry patch, but add better diagnostics soon after:

- store the last `nsapi` open/connect error in the transport
- expose it through `FtpsClient`
- optionally expose a stable transport phase string or retry-hint enum

That change is small, low-risk, and removes the need for string parsing in the application.

## Recommended order of work

1. Remove or debug-gate the post-`STOR` `NOOP` probe.
2. Add a narrow `-3005`-only retry classifier for `store:data-open` failures.
3. Move the retry wrapper into a reusable helper and use it for both base backup files and client snapshot uploads.
4. Add `SIZE` verification for ambiguous final-reply failures.
5. Add `lastNsapiError()` or equivalent to `ArduinoOPTA-FTPS`.
6. After upload retry is stable, consider mirroring the same policy for FTPS restore paths.

## Final assessment

The existing per-file retry proposal is the right direction, but it should land as a **targeted failure classifier plus reusable upload wrapper**, not as a broad "retry all `DataConnectionFailed`" rule.

If implemented that way, the first version will improve FTPS backup resilience without masking configuration bugs, and it will also close the current gap where client snapshot uploads still run without the same pacing and retry protections as the main backup files.
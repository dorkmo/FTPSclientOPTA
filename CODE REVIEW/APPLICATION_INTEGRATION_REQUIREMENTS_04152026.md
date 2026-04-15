# Arduino Application Integration Requirements — ArduinoOPTA-FTPS

**Date:** 2026-04-15  
**Scope:** Review of the FTPS integration-study improvement suggestions plus a concrete plan to reach full working compatibility between ArduinoOPTA-FTPS and a general Arduino application that needs multi-file FTPS workflows.  
**Inputs:** Current `main` branch of ArduinoOPTA-FTPS, existing CODE REVIEW notes, and the user-provided FTPS integration study for a downstream Arduino application.

---

## Executive Summary

The current ArduinoOPTA-FTPS repository already covers the hardest part of a host-application FTPS migration: Explicit FTPS control-channel upgrade, protected passive data transfers, fingerprint trust, imported PEM trust, and best-effort TLS session reuse for data channels.

Full compatibility between the library and a general Arduino application is feasible, but it does **not** require accepting every suggested enhancement as-is. The minimum credible compatibility plan is:

1. Add a small set of missing library capabilities that multi-file Arduino applications materially need.
2. Ensure the integrating application owns FTPS configuration, trust material, and `FtpsClient` bridge logic.
3. Validate archive-size handling, watchdog behavior, and multi-file session behavior on real Opta hardware.

The most important gap on the library side is deterministic remote directory creation. The most important gaps on the application side are FTPS schema/persistence/UI work and the replacement of any plain-FTP transport helpers with `FtpsClient`-based flows.

---

## Definition Of Full Compatibility

For this integration, "full compatibility" means all of the following are true:

- The host application can perform multi-file upload and download workflows over Explicit FTPS.
- The host application can perform manual or automated backup, restore, or sync flows over Explicit FTPS.
- The host application can upload and retrieve archive or history payloads over Explicit FTPS.
- The host application can upload and retrieve manifest-style metadata plus per-item configuration files over Explicit FTPS.
- Fingerprint trust and imported PEM trust both work end to end.
- Remote directory layout does not depend on manual NAS pre-seeding.
- Variable-size downloads either preflight remote size safely or use a streaming path that avoids fixed-buffer failure.
- Connection loss, trust failure, missing directories, and final-reply errors surface as deterministic diagnostics.
- The migration path from any existing plain FTP implementation is explicit rather than accidental.

---

## Suggestion Review

The integration-study suggestions break into four buckets: required now, conditionally required after hardware validation, useful but not required for compatibility, and reject/defer.

| Suggestion | Verdict | Why | Action |
|---|---|---|---|
| `MKD` / directory creation helper | **Required** | Multi-file Arduino applications often upload into nested paths for backups, manifests, config sets, or archives. Relying on pre-created directories is not full compatibility. | Add `mkd()` to the library. Optionally add `ensureDirectory()` or let the host application build a recursive helper on top of `mkd()`. |
| `SIZE` | **Required unless streaming retrieval is adopted immediately** | Downstream applications commonly retrieve variable-size remote content into caller-owned buffers. A size preflight is the smallest safe way to avoid avoidable `RETR` failures. | Add `size()` to the library and use it before large downloads. |
| Stream-oriented transfer API | **Conditional** | If archive or history payloads can exceed practical RAM ceilings, `SIZE` alone only moves the failure earlier. | If measured payload sizes exceed safe in-memory limits, add streaming read/write APIs after the first integration pass. |
| `NOOP` | **Conditional** | Better liveness or keepalive primitive than a cached `connected()` flag. May be needed if servers drop idle control channels during multi-step workflows. | Add only if Opta validation shows real control-channel idling problems. |
| Watchdog callback / poll hook | **Conditional** | Helps would-block polling loops, but does not by itself make TLS handshake calls watchdog-safe. | Evaluate after hardware testing. Measure first, then decide whether the transport needs a callback or handshake refactor. |
| `CWD` | **Optional** | Convenient for repeated relative paths, but host applications can use absolute paths and their own path builders. | Defer behind `MKD` and `SIZE`. |
| `connected()` / `isConnected()` | **Do not add as the primary compatibility fix** | A latched boolean becomes stale after server-side drops. It gives weaker guarantees than `NOOP` or just attempting the next command. | Use `NOOP` if a probe is actually needed. |
| `lastErrorMessage()` | **Optional** | The library's current contract is operation-scoped caller buffers plus `lastError()`. A persistent message accessor adds state and stale-message rules. | Leave out unless repeated logging ergonomics prove painful. |
| `validateServerCert = false` in debug builds | **Reject for now** | The repo currently documents and enforces fail-closed certificate validation. Weakening that contract would create test-versus-production drift in the riskiest part of the API. | Keep the current fail-closed policy. |
| `NLST` / `MLSD` | **Not required** | Applications can use an explicit manifest file when they need to enumerate config payloads. Directory listing is helpful, not necessary. | Defer to a later compatibility release. |
| `DELE` | **Not required** | Current integration requirements do not depend on remote deletion. | Defer. |
| Max concurrent `FtpsClient` docs | **Recommended documentation** | The transport has real heap cost, but the repo is still experimental. Documenting a hard limit before measuring on hardware would be weak. | Document a "one client at a time" assumption after hardware validation. |

---

## Required Compatibility Delta By Layer

### ArduinoOPTA-FTPS: Required Changes

These are the smallest library-side changes needed to support a general Arduino application cleanly.

| Area | Required Code | Likely Files |
|---|---|---|
| Control-command expansion | Add reusable internal helpers so new FTP verbs can be implemented without duplicating control-reply handling. | `src/FtpsClient.cpp` |
| Directory creation | Add `mkd(const char *remoteDir, char *error, size_t errorSize)` and supporting reply handling. | `src/FtpsClient.h`, `src/FtpsClient.cpp`, `src/FtpsErrors.h` |
| Remote size query | Add `size(const char *remotePath, size_t &remoteBytes, char *error, size_t errorSize)` with `213` reply parsing. | `src/FtpsClient.h`, `src/FtpsClient.cpp`, `src/FtpsErrors.h` |
| Optional liveness primitive | Add `noop(char *error, size_t errorSize)` only if interop testing shows it is needed. | `src/FtpsClient.h`, `src/FtpsClient.cpp`, `src/FtpsErrors.h` |
| Documentation | Document directory-creation expectations, buffer-owned transfer semantics, variable-size download guidance, and the single-client assumption for early integrations. | `README.md`, `CHANGELOG.md` |
| Validation harness | Extend at least one live example or the web harness so `MKD` and `SIZE` can be exercised on-device. | `examples/FileZillaLiveTest/FileZillaLiveTest.ino`, `examples/WebHarnessLiveTest/WebHarnessLiveTest.ino` |

### Integrating Arduino Application: Required Changes

These changes live in the host application, not in this repo, but they are required to finish the integration.

| Area | Required Code |
|---|---|
| Config schema | Add FTPS settings to the application config model: security mode, trust mode, validate-cert flag, TLS server name, fingerprint, PEM trust source or path. |
| Persistence | Save and load the FTPS fields if the application stores settings locally. Store imported PEM trust material outside the main JSON blob when practical. |
| Settings surfaces | Extend any existing settings UI, serial menu, local web UI, or API to configure FTPS trust mode, TLS server name, certificate fingerprint, and imported PEM trust. |
| External settings sync | If the application mirrors settings to another subsystem, ensure the FTPS fields are included there too. |
| Transport bridge | Replace any plain-FTP helper path with a bridge layer that calls `FtpsClient.begin()`, `connect()`, `store()`, `retrieve()`, `quit()`, plus `mkd()` / `size()` as needed. |
| Remote directory prep | Ensure backup, manifest, config, and archive paths are created before `STOR`. |
| Variable-size download strategy | Use `size()` to reject or handle oversized downloads before `RETR`, or switch large-payload paths to a streaming design if the files can exceed a safe RAM ceiling. |
| Watchdog handling | Continue kicking the watchdog around FTPS operations and confirm on-device whether library polling loops or TLS handshakes require a stronger hook. |
| Transition policy | Keep a plain FTP path only if mixed-field deployments still require it. Otherwise choose an explicit FTPS cutover and remove dual-path complexity later. |

---

## Recommended Public API Additions In ArduinoOPTA-FTPS

The smallest useful public delta for the library is:

```cpp
bool mkd(const char *remoteDir, char *error, size_t errorSize);
bool size(const char *remotePath, size_t &remoteBytes,
          char *error, size_t errorSize);

// Add only if interop testing proves a need.
bool noop(char *error, size_t errorSize);
```

This preserves the current Arduino-style API shape and avoids adding stateful accessors that do not materially improve reliability.

If later testing proves that large archive or history downloads routinely exceed a safe in-memory ceiling, the next API addition should be a streaming retrieval path rather than more boolean state accessors.

---

## Library Implementation Plan

### Phase 1 - Internal Command Refactor

Before adding new public verbs, factor the control-channel command path so `STOR`, `RETR`, `MKD`, `SIZE`, and optional `NOOP` all share the same command-formatting and reply-handling rules.

**Code goals:**

- Extract a helper to send a formatted command and return the reply code plus reply text.
- Avoid copy-pasting command-size validation and final-error propagation.
- Keep existing `store()` and `retrieve()` behavior unchanged while the helper is introduced.

**Primary files:**

- `src/FtpsClient.cpp`

**Exit criteria:**

- Existing upload and download examples still compile conceptually against the unchanged API.
- New command helpers are ready for `MKD` and `SIZE` without duplicating reply logic.

### Phase 2 - Add `mkd()`

Implement remote directory creation in the library.

**Code goals:**

- Add `mkd()` to the public client.
- Accept success replies such as `257` and handle server "already exists" responses deterministically.
- Add specific error codes if the existing `TransferFailed` bucket becomes too generic.

**Primary files:**

- `src/FtpsClient.h`
- `src/FtpsClient.cpp`
- `src/FtpsErrors.h`

**Host-application follow-on code:**

- Add a helper that creates the path tree for:
  - base backup directory
  - manifest directory
  - per-item config directory
  - archive or history directory

**Exit criteria:**

- A live example can create a missing remote directory on FileZilla or the target NAS.
- The host application no longer depends on manual directory pre-creation.

### Phase 3 - Add `size()`

Implement remote file-size preflight in the library.

**Code goals:**

- Add `size()` to the public client.
- Parse `213` replies into a caller-owned `size_t`.
- Fail clearly when the server rejects `SIZE` or returns malformed output.

**Primary files:**

- `src/FtpsClient.h`
- `src/FtpsClient.cpp`
- `src/FtpsErrors.h`

**Host-application follow-on code:**

- Query remote size before large `RETR` operations.
- Reject downloads that exceed the current buffer ceiling with a clear user-facing error.
- Decide, based on measured payload sizes, whether the current fixed-buffer path is sufficient.

**Exit criteria:**

- The host application can prevent predictable large-file download failures before opening the FTPS data channel.

### Phase 4 - Conditional `noop()` And Watchdog Decision

Do not add keepalive or poll hooks blindly. Use hardware data first.

**Code goals:**

- Run multi-file workflows against the target FTPS servers.
- Measure whether the control channel idles out between transfers.
- Measure whether watchdog servicing is sufficient when it occurs only around library calls.

**Decision branch:**

- If control idling is a real failure mode, add `noop()`.
- If watchdog starvation occurs inside library wait loops, add a callback hook or revisit transport blocking behavior.
- If neither happens, keep the public API smaller.

### Phase 5 - Conditional Streaming Transfer Support

Only add streaming transfer APIs if payload-size measurements prove the current buffer-owned design is not enough.

**Code goals:**

- Prefer a callback-based streaming read path over one more large buffer.
- Keep the existing `retrieve()` and `store()` calls for small files and examples.
- Use the streaming path only for large archive or history payloads.

**Exit criteria:**

- Large downloads no longer depend on oversized static buffers.

---

## Host Application Integration Plan

### Phase A - Schema And Persistence

Add the FTPS configuration fields to the application config model and persistence layer.

**Required config fields:**

- `ftpSecurityMode`
- `ftpTlsTrustMode`
- `ftpValidateServerCert`
- `ftpTlsServerName`
- `ftpTlsFingerprint`
- PEM trust path or equivalent loaded PEM source

**Exit criteria:**

- Legacy configs still load safely.
- New FTPS fields persist cleanly.
- PEM trust material survives reboot.

### Phase B - Settings Surface And External Sync

Expose the FTPS settings through the application's existing configuration flows.

**Required behaviors:**

- Any settings UI or API can select plain FTP versus Explicit FTPS during transition.
- Any settings UI or API can select fingerprint trust versus imported PEM trust.
- Existing configuration endpoints or menus return and accept all FTPS settings.
- Any external configuration-sync layer includes the FTPS fields.

**Exit criteria:**

- Operators can configure FTPS end to end without manual source edits.

### Phase C - FTPS Bridge And Workflow Migration

Replace the current plain-FTP transport helpers with a bridge that uses the library.

**Bridge responsibilities:**

- Initialize the client with `Ethernet.getNetwork()`.
- Map application settings to `FtpsServerConfig`.
- Connect, upload, download, and quit through `FtpsClient`.
- Create remote directories before uploads.
- Preflight remote size before large archive or history downloads.

**Workflow categories to migrate:**

- multi-file backup workflow
- multi-file restore workflow
- manifest upload and download workflow
- archive or history upload workflow
- archive or history download workflow

**Exit criteria:**

- Every existing FTP workflow has an FTPS-backed code path.

### Phase D - Reliability Hardening

Validate the system under real workload conditions.

**Required checks:**

- Multi-file session success against PR4100, FileZilla Server, and vsftpd.
- Data-channel TLS reuse behavior on each server.
- Oversized download behavior.
- Missing-directory behavior.
- Trust failure behavior for bad fingerprint, bad PEM, and hostname mismatch.
- Watchdog behavior during large transfers and handshakes.

**Exit criteria:**

- The FTPS path is field-usable, not just lab-usable.

---

## What Should Not Be Treated As A Compatibility Blocker

The following items are worth tracking, but they should not delay the first complete integration:

- `CWD`
- `lastErrorMessage()`
- `NLST` / `MLSD`
- `DELE`
- connection pooling across `quit()` / `connect()` cycles
- a public `connected()` / `isConnected()` accessor

These features improve ergonomics or broaden the library, but they are not required to make current multi-file Arduino application workflows function over FTPS.

---

## Acceptance Matrix

The integration is complete only when the following matrix passes on real Opta hardware:

| Workflow | FTPS Required | Notes |
|---|---|---|
| Multi-file backup | Yes | Includes missing-directory creation |
| Multi-file restore | Yes | Includes `SIZE` preflight or streaming path |
| Periodic or event-driven sync | Yes | Must survive repeated multi-file sessions |
| Boot-time restore, if used | Yes | Must fail cleanly on trust or network errors |
| Manifest upload and download | Yes | Directory creation plus repeated transfer stability |
| Archive or history upload | Yes | Nested path creation required |
| Archive or history download | Yes | Size-safe retrieval required |
| Fingerprint trust | Yes | Positive and negative cases |
| Imported PEM trust | Yes | Positive and negative cases |
| PR4100 interop | Yes | Reference NAS |
| FileZilla interop | Yes | Development reference server |
| vsftpd interop | Yes | Broader standards validation |

---

## Recommended Work Order

1. Add `mkd()` to ArduinoOPTA-FTPS.
2. Add `size()` to ArduinoOPTA-FTPS.
3. Extend one live example or harness to validate both commands on Opta hardware.
4. Add FTPS schema, persistence, settings-surface support, and any external-sync field coverage to the host application.
5. Add the `FtpsClient` bridge and migrate backup, restore, manifest, and archive workflows.
6. Measure payload sizes and decide whether `SIZE` is sufficient or streaming transfer support is needed.
7. Only after interop testing, decide whether `NOOP` or a watchdog callback should be added.

---

## Final Recommendation

Use ArduinoOPTA-FTPS as the FTPS transport layer for general Arduino applications that need Explicit FTPS plus protected passive data transfers. Treat `MKD` and `SIZE` as the first library additions required for clean integration. Keep the current fail-closed certificate-validation policy. Defer `connected()`, `lastErrorMessage()`, listing commands, and insecure debug TLS modes because they do not materially improve compatibility and would dilute the current API contract.

If payload-size measurements prove the fixed-buffer retrieval model is too tight, make streaming transfer support the next major library feature. Until then, keep the compatibility plan focused on the smaller set of additions that directly unblock host-application integration.
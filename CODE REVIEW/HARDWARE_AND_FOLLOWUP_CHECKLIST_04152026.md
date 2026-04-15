# Hardware And Follow-Up Checklist — ArduinoOPTA-FTPS

**Date:** 2026-04-15  
**Purpose:** Consolidate the remaining hardware validation and post-validation code or documentation updates into one checklist now that the first library implementation pass is complete.

---

## Current Answer

Yes. There is still meaningful work left, but it is now concentrated in two areas:

1. Real Opta hardware validation across the target FTPS servers.
2. Code or documentation updates that depend on those hardware results.

The library-side implementation work for `mkd()` and `size()` is done. The remaining decisions should be driven by test evidence, not by more speculative API additions.

---

## Immediate Remaining Work

- [ ] Run Opta hardware validation against FileZilla Server.
- [ ] Run Opta hardware validation against the PR4100.
- [ ] Run Opta hardware validation against vsftpd.
- [ ] Record whether data-channel TLS session reuse is required on each server.
- [ ] Record whether `mkd()` and `size()` behave consistently across all three servers.
- [ ] Decide whether `NOOP`, a watchdog callback, or streaming transfer support is actually needed.
- [ ] Update release-facing docs after the hardware results are known.

---

## Test Environment Prep

- [ ] Confirm the Arduino Opta board package version in use.
- [ ] Confirm the actual board target and flash procedure to be used for validation.
- [ ] Confirm whether `arduino-cli` or the Arduino IDE will be used for compile validation.
- [ ] Capture the FTPS server configuration for each target server before testing.
- [ ] Confirm passive mode is enabled on each server.
- [ ] Confirm Explicit TLS is enabled on each server.
- [ ] Capture test credentials and trust material for both fingerprint and imported-cert runs.

---

## FileZilla Server Validation

Use [examples/FileZillaLiveTest/FileZillaLiveTest.ino](examples/FileZillaLiveTest/FileZillaLiveTest.ino) as the first full validation path.

- [ ] Compile the sketch for Arduino Opta.
- [ ] Run with fingerprint trust.
- [ ] Confirm control-channel connect and login succeed.
- [ ] Confirm `mkd()` succeeds for the parent directory.
- [ ] Confirm `mkd()` succeeds for the nested directory.
- [ ] Confirm `store()` succeeds.
- [ ] Confirm `size()` returns the expected byte count.
- [ ] Confirm `retrieve()` succeeds.
- [ ] Confirm `quit()` returns a clean result.
- [ ] Re-run with imported PEM trust.
- [ ] Record serial output for a successful run.
- [ ] Record the first failure message and `FtpsError` value if anything fails.

---

## PR4100 Validation

Use [FtpsSpikeTest/FtpsSpikeTest.ino](FtpsSpikeTest/FtpsSpikeTest.ino) and then the FileZilla live-test pattern as needed.

- [ ] Confirm TCP control connection and 220 banner.
- [ ] Confirm `AUTH TLS` succeeds.
- [ ] Confirm control-channel TLS handshake succeeds.
- [ ] Confirm `PBSZ 0` and `PROT P` succeed.
- [ ] Confirm login succeeds over the encrypted control channel.
- [ ] Confirm `PASV` returns a parseable response.
- [ ] Confirm data-channel TLS handshake succeeds.
- [ ] Confirm `store()` succeeds.
- [ ] Confirm `retrieve()` succeeds.
- [ ] Confirm `mkd()` works for a missing nested path.
- [ ] Confirm `size()` returns the expected byte count.
- [ ] Record whether the NAS requires strict TLS session reuse.
- [ ] Record handshake timing and any RAM observations.

---

## vsftpd Validation

- [ ] Confirm the server configuration around FTPS and `require_ssl_reuse`.
- [ ] Run a fingerprint-trust test.
- [ ] Confirm upload and download succeed.
- [ ] Confirm `mkd()` and `size()` succeed.
- [ ] Record whether the current best-effort session reuse is sufficient.
- [ ] If data-channel TLS fails, capture the exact server setting and Opta-side error.

---

## Negative-Case Validation

- [ ] Wrong password returns a login-specific failure.
- [ ] Wrong fingerprint returns a certificate-specific failure.
- [ ] Malformed fingerprint is rejected before connect.
- [ ] Wrong or stale imported PEM trust data fails cleanly.
- [ ] Imported-cert mode with IP host and no `tlsServerName` fails clearly.
- [ ] `mkd()` failure on a forbidden or invalid path returns a directory-create-specific failure.
- [ ] `size()` failure on a missing file returns a size-query-specific failure.
- [ ] Malformed `SIZE` replies are surfaced clearly.
- [ ] Oversized download preflight behavior is documented.

---

## Stability Checks

- [ ] Repeated connect, transfer, and quit cycles succeed.
- [ ] Repeated `mkd()` calls on the same path behave deterministically.
- [ ] Repeated `size()` calls on the same file behave deterministically.
- [ ] Temporary network interruption behavior is characterized.
- [ ] Failure paths still close sockets cleanly.
- [ ] Large transfers stay within the current timeout budget.

---

## Hardware-Driven Code Decisions

Only make these changes if hardware testing shows they are actually needed.

### Keepalive / Liveness

- [ ] Decide whether a public `noop()` method is needed.
- [ ] If yes, implement `noop()` and add live-example coverage.
- [ ] If no, record that the current control-session behavior is sufficient.

### Watchdog Handling

- [ ] Decide whether watchdog servicing around library calls is enough.
- [ ] If not, decide whether a simple polling callback is sufficient.
- [ ] If not, revisit the transport blocking behavior during TLS handshake.

### Streaming Transfers

- [ ] Measure whether real application payloads exceed safe RAM ceilings.
- [ ] If not, keep buffer-owned `retrieve()` as the primary path.
- [ ] If yes, design a streaming transfer API and add it as the next code phase.

---

## Post-Validation Documentation Updates

- [ ] Update [README.md](README.md) with tested status instead of planned status where appropriate.
- [ ] Update [CHANGELOG.md](CHANGELOG.md) with the hardware validation results.
- [ ] Update [CODE REVIEW/FTPS_IMPLEMENTATION_CHECKLIST_04132026.md](CODE%20REVIEW/FTPS_IMPLEMENTATION_CHECKLIST_04132026.md) to check off the hardware-complete items.
- [ ] Update [CODE REVIEW/APPLICATION_INTEGRATION_REQUIREMENTS_04152026.md](CODE%20REVIEW/APPLICATION_INTEGRATION_REQUIREMENTS_04152026.md) with any findings that change the integration guidance.
- [ ] Add server-specific notes if one of the reference servers has non-obvious compatibility constraints.
- [ ] Document whether the provisional one-client-at-a-time assumption still stands.
- [ ] Document whether `mkd()` and `size()` are validated across all reference servers or only a subset.

---

## Optional Follow-Up Code Work

These are reasonable next steps, but they should stay behind the validation gates above.

- [ ] Add a dedicated imported-cert live example.
- [ ] Add a dedicated fingerprint-only trust example.
- [ ] Add WebHarness coverage for `mkd()` and `size()`.
- [ ] Add a documented compatibility matrix across reference servers.
- [ ] Add a release-readiness checklist once the hardware runs are complete.

---

## Exit Criteria

This phase is complete when all of the following are true:

- [ ] FileZilla, PR4100, and vsftpd each have a recorded Opta validation result.
- [ ] `mkd()` and `size()` have real hardware results, not just editor validation.
- [ ] The need for `NOOP`, watchdog callbacks, and streaming transfers is evidence-based.
- [ ] README and release-facing docs match observed behavior.
- [ ] Remaining open work is clearly either release hardening or v2 scope, not unresolved v1 ambiguity.
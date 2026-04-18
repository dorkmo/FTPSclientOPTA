# Multi-File FTPS Backup — Follow-Up Optimizations

**Date:** 2026-04-17
**Status:** Multi-file backup is verified working on Arduino Opta with
`proc=8 failed=0`. This document tracks optional optimizations that
trade complexity for either faster backup runs or improved resilience.

## Current Behavior (Baseline)

Verified live on Arduino Opta + pyftpdlib FTPS server:

- 8 files uploaded successfully in a single session.
- Total backup time: **~656 seconds (~11 minutes)** for 8 files.
- Per-file cost: ~5–10 seconds of actual transport work + a 65 second
  inter-file `TIME_WAIT` drain wait.
- Watchdog never trips (30 s STM32H7 hardware watchdog is kicked every
  100 ms during the wait).

## Why Backups Are Slow on Opta

Two limits in the precompiled `libmbed.a` shipped with `arduino:mbed_opta`
4.5.0 dominate runtime:

1. **`MBED_CONF_LWIP_SOCKET_MAX = 4`** — only 4 TCP PCBs exist on the
   entire device. Editing `variants/OPTA/mbed_config.h` has no effect
   because the LWIP code that reads those macros is already compiled
   into `libmbed.a`.
2. **`SO_LINGER` returns `NSAPI_ERROR_UNSUPPORTED` (-3002)** — every
   close goes through full FIN/ACK + ~60 s `TIME_WAIT`.

With a `LISTEN` socket consuming 1 PCB and the active control channel
consuming another, only 2 slots remain. Opening a new data socket while
the previous one is still in `TIME_WAIT` fails with
`NSAPI_ERROR_NO_SOCKET` (-3005). Waiting ~65 s between files lets the
old data socket fully release before the next one opens.

## Optimization Candidates

### 1. Tune the inter-file delay

| Delay | Risk | Backup time (8 files) |
| ----- | ---- | --------------------- |
| 65 s  | None observed | ~656 s (~11 min) |
| 45 s  | Low — typical LWIP `TIME_WAIT` is `2 * MSL`, default MSL is 30 s but real-world drains often complete earlier | ~440 s (~7 min) |
| 30 s  | Medium — within `2 * MSL` window, may intermittently fail with -3005 | ~330 s (~5.5 min) |
| 10 s  | High — almost certainly fails on file 2 or 3 | ~165 s (~2.75 min) |

**Recommendation:** instrument the existing `xport:open-failed:-3005`
trace into a counter and try 45 s. If no failures across 10 consecutive
backups, lower to 30 s. Don't go below 30 s without a different
strategy.

### 2. Per-file retry with backoff

Today, when `xport:open-failed:-3005` fires, `performFtpBackupDetailed()`
sets `abortRemainingTransfers = true` and the entire backup ends with a
partial result. A bounded retry would let the rest of the backup
continue:

- On `-3005` open failure, sleep an additional 30 s, then retry the
  same file. Maximum 2 retries per file.
- If both retries fail, mark that file failed but continue to the
  next file (do **not** abort).
- Today `result.errorMessage` already records the first failure; the
  retry path should append a comma-separated list to `result.failedFiles`
  for completeness.

This is small (~30 lines in `performFtpBackupDetailed`) and harmless: at
worst it wastes 60 extra seconds per truly stuck file. It also recovers
from transient browser-poll storms that briefly steal a PCB.

**Verdict:** worth doing. Tracked as todo item.

### 3. Background backup ticker

Today `/api/ftp-backup` blocks for ~11 minutes. A browser request will
either time out or show a long spinner. Better:

- `/api/ftp-backup` flips `gPendingFtpBackup = true` and returns
  `202 Accepted` immediately with a job ID.
- The main loop runs the backup in slices and persists progress to
  `/fs/backup_progress.json`.
- A new `/api/ftp-backup/status?id=...` endpoint returns the live
  state (`files_done`, `files_total`, `current_file`, `last_error`).

Application-side change only; library is already non-blocking enough.
Bigger refactor — defer until a user actually complains.

### 4. Bundle multiple files into a single STOR

Concatenate all backup files into a single tar/zip stream (or just a
JSON envelope) and STOR it as one upload. Drops the per-file
`TIME_WAIT` cost entirely; backup of "8 files" becomes a single transfer.

- **Pro:** ~10 s total instead of 11 minutes.
- **Con:** server-side restore is no longer file-for-file compatible
  with the existing layout. Restore would need to know how to unpack.
- **Con:** Opta cannot host an in-memory tar of arbitrary size; the
  current 2 KB per-file buffer would need to grow or stream.

Probably the right long-term answer if backup throughput becomes a
priority. Not urgent.

### 5. Move LWIP to a custom-rebuilt `libmbed.a`

Possible in principle (clone `ArduinoCore-mbed`, edit
`mbed-os/connectivity/lwipstack/lwipopts_default.h`, rebuild). Costs:

- ~30 minutes of native toolchain setup (gcc-arm-none-eabi).
- Has to be repeated per machine and per core upgrade.
- No upstream path to Arduino's official binary.

Not worth doing unless 4 PCBs becomes a hard wall for some other
feature (e.g. concurrent SMTP + FTPS).

### 6. Warm `TIME_WAIT` mitigation via SO_REUSEADDR

`setsockopt(SO_REUSEADDR)` on the *outbound* connect side is meaningless
in standard BSD semantics; it only affects bind. Not applicable here.

### 7. Switch backup transport to plain FTP (not FTPS) on a trusted LAN

Plain FTP would still hit the same 4-PCB pool and the same `TIME_WAIT`
behavior, so this saves nothing on the socket front. Only worth doing
if the TLS handshake itself is the bottleneck — it isn't here (~1.5 s
per handshake out of ~70 s per file).

## Recommended Next Steps (in priority order)

1. **Add per-file retry on `-3005`** (item 2). Highest value:lowest
   complexity.
2. **Tune inter-file delay to 45 s** (item 1) once retry is in place to
   provide a safety net. Cuts backup time by ~30 %.
3. **Background ticker** (item 3) only if a user reports the long
   blocking call is a problem.
4. **Bundle to single STOR** (item 4) if backup throughput becomes a
   product requirement.
5. Skip items 5, 6, 7 unless circumstances change.

## Cross-References

- [CHANGELOG.md](../CHANGELOG.md) — Unreleased section documents the
  close-path fix and multi-file verification.
- [README.md](../README.md) — Limitations section documents the
  per-machine LWIP constraints.
- [SOCKET_CLOSE_HANG_ANALYSIS_04172026.md](SOCKET_CLOSE_HANG_ANALYSIS_04172026.md)
  — original hang root-cause analysis.
- `OPTA_LWIP_BACKUP_RECIPE_04172026.md` — application-side recipe for
  releasing the listening server socket and pacing transfers.

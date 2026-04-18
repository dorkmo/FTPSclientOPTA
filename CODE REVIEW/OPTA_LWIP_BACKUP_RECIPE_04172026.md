# Arduino Opta — Multi-File FTPS Backup Recipe

**Date:** 2026-04-17
**Audience:** Application developers integrating ArduinoOPTA-FTPS into
an Opta sketch that also hosts a web UI or other TCP services.

## Problem

The Mbed OS LWIP build shipped with `arduino:mbed_opta` 4.5.0 imposes
two hard limits that affect any application doing back-to-back FTPS
uploads:

- **`MBED_CONF_LWIP_SOCKET_MAX = 4`** is baked into the precompiled
  `libmbed.a`. Editing `variants/OPTA/mbed_config.h` has **no** effect.
- **`SO_LINGER` is unsupported** (returns `NSAPI_ERROR_UNSUPPORTED` /
  `-3002`). Every closed TCP socket sits in `TIME_WAIT` for ~60 s
  instead of being hard-reset.

Together these mean that an application with an `EthernetServer`
listening for browser connections has at most 1–2 PCB slots free for
FTPS work. A naive backup loop that uploads 5+ files will fail on the
2nd or 3rd file with `NSAPI_ERROR_NO_SOCKET` (`-3005`) reported by the
library as `xport:open-failed:-3005`.

## Recipe

This is the pattern that achieves `proc=8 failed=0` on real hardware
against pyftpdlib.

### 1. Release the LISTEN socket during backup

Stop the `EthernetServer` immediately before calling the backup
function and restart it immediately after. This frees the LISTEN PCB
for the duration of the backup window:

```cpp
extern EthernetServer gWebServer;

void runBackup() {
  gWebServer.end();   // Free LISTEN PCB
  performFtpBackup();
  gWebServer.begin(); // Restore web server
}
```

If the application has already-accepted browser clients holding sockets
when backup starts, also drop those (a `volatile bool gBackupInProgress`
flag checked at the top of the request handler that calls
`client.stop(); return;` is enough).

### 2. Wait one full `TIME_WAIT` interval between files

Add a 65-second wait between successive `STOR` operations. The wait
must service the watchdog or the device will reset:

```cpp
for (size_t i = 0; i < fileCount; ++i) {
  if (i > 0) {
    uint32_t waitStart = millis();
    while (millis() - waitStart < 65000UL) {
      kickWatchdog();   // 30 s STM32H7 watchdog
      delay(100);
    }
  }
  ftpsClient.store(remotePath, buffer, length, ...);
}
```

The 65 s constant is `2 * MSL` (LWIP default `MSL = 30 s`) plus a 5 s
margin. Tuning lower than 30 s is unsafe; see the Follow-Ups document
for the trade-off table.

### 3. Surface `-3005` failures and consider retrying

The library now traces `xport:open-failed:-3005` whenever
`socket.open()` fails because the LWIP pool is exhausted. Treat this as
a recoverable condition: wait an additional 30 s and retry the same
file before marking it failed.

### 4. Don't open a browser tab during backup

If the application's web UI uses any kind of polling AJAX (e.g. a
status page that refreshes every 5 s), close the tab before kicking
off the backup. Each accepted browser request costs 1 PCB and can
cause `-3005` on the FTPS data channel even with the LISTEN socket
released.

## Verification Trace

A successful 8-file backup produces traces in this shape (per file):

```
FTPS phase: store:entry
... (PASV + handshake + write) ...
FTPS phase: xport:linger-unsupported:-3002   <-- expected on Opta
FTPS phase: xport:cleanup:tcp-close
FTPS phase: xport:cleanup:tcp-closed
FTPS phase: xport:cleanup:tls-delete
FTPS phase: xport:cleanup:tls-deleted
FTPS phase: xport:cleanup:tcp-delete
FTPS phase: xport:cleanup:tcp-deleted
FTPS phase: store:done
FTP backup: <remote path>
FTPS phase: inter-file:wait-tw       <-- 65 s drain
FTPS phase: inter-file:wait-done
```

If `xport:open-failed:-3005` ever appears, either the inter-file delay
is too short, the LISTEN socket wasn't released, or a browser request
is consuming a PCB.

## Cross-References

- [README.md](../README.md) — Limitations section.
- [CHANGELOG.md](../CHANGELOG.md) — multi-file verification entry.
- `MULTI_FILE_BACKUP_FOLLOWUPS_04172026.md` — optional optimizations.

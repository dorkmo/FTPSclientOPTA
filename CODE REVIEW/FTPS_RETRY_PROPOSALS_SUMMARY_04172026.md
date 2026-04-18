# FTPS File Transfer Retry Proposals & Improvements

**Date:** April 17, 2026
**Repositories Reviewed:** Tank Alarm (`ArduinoSMSTankAlarm`), ArduinoOPTA-FTPS

This document synthesizes findings and proposes improvements for handling FTPS file transfer failures, specifically addressing the hardware watchdog resets and socket pool exhaustion issues observed on the Arduino Opta platform.

## 1. Root Cause Summary
The primary source of FTPS transfer failures is a hardware watchdog reset caused by a blocking implementation of `TCPSocket::close()` within the `mbedOS` core. 

To prevent this hang, a temporary workaround was introduced in the `ArduinoOPTA-FTPS` library to intentionally leak LwIP socket handles (`_dataSocket`, `_controlSocket`) rather than cleanly closing them. However, because the Opta platform has a very small, hardcoded socket pool (`MBED_CONF_LWIP_SOCKET_MAX = 4`), back-to-back file transfers quickly exhaust the pool. Sockets languish in the `TIME_WAIT` state, leading to subsequent data channel allocations failing with error `-3005` (`NSAPI_ERROR_NO_SOCKET` or `DataConnectionFailed`).

## 2. Per-File Retry Logic Recommendations
To mitigate socket exhaustion at the application level, the following bounded retry logic should be implemented for multi-file batches:

*   **Error-Specific Detection:** Only initiate retries if the library returns `DataConnectionFailed` or `-3005`. This indicates transient data-pool exhaustion. If the error indicates a dead control channel (e.g., `ControlIoFailed`), the entire session should be aborted.
*   **Bounded Retry Loop:** Allow a maximum of **3 attempts** (2 retries) per file.
*   **Backoff Delay:** Introduce a **30,000 ms (30-second)** backoff between retries. This delay is critically required to give the LwIP pool time to drain its `TIME_WAIT` sockets. *Ensure the hardware watchdog is serviced during this delay.*
*   **Graceful Degradation:** If a file fails after 3 attempts, mark that specific file as failed, log the error, and **continue to the next file**. Do not abort the entire batch unless the control channel itself has failed.

## 3. Batch and Architectural Improvements
Beyond per-file retries, several architectural changes are recommended to improve the reliability and user experience of the backup process:

*   **Single Batch Payload (Concatenation/Tar):** The most robust long-term application fix is to concatenate or archive all backup files into a single large payload before transfer. This requires only one data socket for the entire backup, bypassing the multi-file `TIME_WAIT` socket exhaustion entirely and reducing backup time from ~11 minutes to ~10 seconds.
*   **Scheduled Background Jobs (Nightly Backups):** Because multi-file backups currently block the web UI, schedule these processes autonomously during off-hours (e.g., 2:00 AM) when socket constraints and UI interaction are minimal.
*   **Inter-File Delay Tuning:** Once the per-file retry logic is implemented, the conservative 65-second delay currently used between files can be reduced to **30-45 seconds**, significantly speeding up the batch process.

## 4. Core Transport Fixes (Library Level)
To achieve true stability, the intentional socket leak in the core transport library must be resolved. 

*   **Investigate `SO_LINGER=0`:** Try setting `SO_LINGER=0` via `setsockopt` before closing the socket. This forces a TCP `RST` (dropping the connection immediately) instead of entering a graceful `FIN` handshake. This may circumvent the Mbed OS blocking behavior on `close()` and allow for clean socket disposal without triggering the watchdog.
*   **Avoid "Reconnect-Per-File":** A "reconnect per file" strategy (opening and closing a new FTPS session for every file) is strongly discouraged. Because the underlying library currently abandons both control and data sockets, iterating full sessions per file will leak control sockets even faster, accelerating the point of failure.
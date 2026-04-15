# ArduinoOPTA-FTPS

ArduinoOPTA-FTPS is an experimental repository for a general-purpose FTPS client library for Arduino Opta devices.

The goal is to provide a reusable, Opta-first FTPS library built on the board's Ethernet and Mbed networking stack, with initial validation targeted against a WD My Cloud PR4100, vsftpd, and FileZilla Server, and designed to support other standards-compliant FTPS servers over time.

> [!IMPORTANT]
> This repository now includes early library scaffolding (public headers, transport interface, and example sketches), but FTPS transport/client logic is still stubbed. Treat FTPS support as experimental until the Opta hardware spike is completed and implementation phases are finished.

## Project Goals

- Provide a small Arduino-style FTPS client for Arduino Opta.
- Ship Explicit FTPS (`AUTH TLS`) over Ethernet first.
- Support protected passive-mode transfers (`PBSZ 0`, `PROT P`) for upload and download.
- Keep certificate handling practical for field deployments.
- Keep the session and transport model flexible enough to add Implicit FTPS and optional active mode later.
- Stay general enough to support servers beyond the initial reference NAS.

## Planned v1 Scope

- Explicit FTPS only
- Passive mode only
- Binary transfers only
- Upload and download primitives
- SHA-256 fingerprint pinning
- Imported PEM certificate trust
- FTPS-specific error reporting suitable for field diagnostics

## Not in Scope for v1

- Implicit FTPS
- Active mode
- Recursive sync
- Delete, rename, mkdir, or broader file-management helpers
- Product-specific UI, config storage, or backup policy
- Broad multi-board support claims before hardware validation

## Candidate v2 Scope

- Explicit and Implicit FTPS modes under a common client API
- Optional active-mode transfers in addition to passive mode
- Capability discovery and directory helpers such as `FEAT`, `PWD`, `MLSD`, and `NLST`
- File-management helpers such as `DELE`, `RNFR`/`RNTO`, `MKD`, and `RMD`
- Stream-oriented transfer APIs in addition to buffer-based helpers
- Broader certificate and trust-management options
- Expanded interoperability testing across more FTPS servers
- Possible support for additional Mbed-based Arduino targets after Opta support is stable

Active mode is worth keeping on the v2 backlog, but it should stay behind Implicit FTPS and passive-mode hardening. It adds firewall and NAT complexity, and it is less likely to matter for the initial Opta-to-NAS deployment patterns this library is targeting.

## Compatible Hardware

- Arduino Opta only for v1
- Intended minimum core: `arduino:mbed_opta` 4.5.0 or later
- Initial network path: Ethernet via `PortentaEthernet` and `Ethernet`

## Reference Servers

The v1 validation targets are:

- **WD My Cloud PR4100** — NAS with built-in FTPS support
- **vsftpd** — widely deployed Linux FTPS server
- **FileZilla Server** — cross-platform FTPS server with GUI management

These are reference servers, not a guarantee of universal FTPS compatibility. Broader compatibility claims should only be made after real interop testing.

## Expected Dependencies

When the first implementation lands, the library is expected to rely on:

- Arduino Opta board support package
- `PortentaEthernet` and `Ethernet`
- Mbed networking interfaces exposed by the Opta core, including `NetworkInterface`, `TCPSocket`, and `TLSSocketWrapper`

## Installation

This repository is not yet published to Arduino Library Manager.

For development, it can be used as a local library checkout. Keep in mind that the current `src/` content is scaffolding-only and not a production-ready FTPS implementation yet.

## Current API Direction

The public API is intended to stay small and Arduino-style. The current scaffold direction looks like this:

```cpp
// Illustrative scaffold usage.
// Current methods are declared in src/FtpsClient.h but still stubbed.

FtpsServerConfig config;
config.host = "192.168.1.100";
config.port = 21;
config.user = "user";
config.password = "pass";
config.tlsServerName = "nas.local";
config.trustMode = FtpsTrustMode::Fingerprint;
config.fingerprint = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
config.validateServerCert = true;
config.passiveMode = true;

FtpsClient client;
char error[96];

if (!client.begin(Ethernet.getNetwork(), error, sizeof(error))) {
	Serial.println(error);
	return;
}

if (client.connect(config, error, sizeof(error))) {
	const uint8_t payload[] = "{}";
	client.store(
			"/backup/config.json",
			payload,
			sizeof(payload) - 1,
			error,
			sizeof(error));

	uint8_t buffer[64] = {};
	size_t bytesRead = 0;
	client.retrieve(
			"/backup/config.json",
			buffer,
			sizeof(buffer),
			bytesRead,
			error,
			sizeof(error));

	client.quit();
}
```

The public API surface is `begin()`, `connect()`, `store()`, `retrieve()`, `quit()`, and `lastError()`. `begin()` initializes the transport layer with the Mbed `NetworkInterface` and must be called once before `connect()`. `lastError()` returns an `FtpsError` enum for programmatic error handling alongside the human-readable `char*` error buffer.

## Trust Model

- SHA-256 leaf-certificate fingerprint pinning
- Imported PEM certificate trust
- Certificate validation enabled by default
- No silent fallback from FTPS to plaintext FTP

## Current and Planned Examples

The repository currently includes:

- `examples/BasicUpload/BasicUpload.ino`
- `examples/BasicDownload/BasicDownload.ino`
- `FtpsSpikeTest/FtpsSpikeTest.ino`

Planned follow-up examples after transport/client implementation lands:

- Trust-mode-focused fingerprint validation example
- Trust-mode-focused imported PEM validation example
- PR4100 reference setup
- vsftpd reference setup
- FileZilla Server reference setup

## Current Status

This repository currently contains planning docs plus scaffold code:

- Architecture and implementation notes
- An execution checklist for the FTPS migration
- A repository bootstrap and extraction plan
- A Phase 0 transport spike plan for Arduino Opta hardware
- Library skeleton files in `src/` (`FtpsClient`, `FtpsTypes`, `FtpsErrors`, trust/transport stubs)
- Upload/download scaffold examples and a full spike sketch

Before a first experimental release, the project still needs:

1. A real Opta hardware spike proving `AUTH TLS` control upgrade with `TLSSocketWrapper`
2. Protected passive data-channel validation
3. Functional (non-stub) upload and download implementations
4. Fingerprint and imported-certificate validation tests

## Limitations

- Experimental repository; no released library yet
- Arduino Opta is the only planned supported board for v1
- Explicit FTPS only
- Passive mode only
- Compatibility beyond the three reference servers is not yet claimed
- Servers that enforce unsupported TLS session reuse behavior may require additional transport work

## Project Documentation

- [Implementation note](CODE%20REVIEW/FTPS_IMPLEMENTATION_04132026.md)
- [Implementation checklist](CODE%20REVIEW/FTPS_IMPLEMENTATION_CHECKLIST_04132026.md)
- [Repository creation plan](CODE%20REVIEW/FTPS_REPOSITORY_REVIEW_04132026.md)
- [Phase 0 spike plan](CODE%20REVIEW/FTPS_SPIKE_PLAN_04142026.md)

## License

This repository uses the CC0 1.0 license. See [LICENSE](LICENSE).

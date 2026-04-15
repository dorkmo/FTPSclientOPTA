// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0

#ifndef FTPS_TRUST_H
#define FTPS_TRUST_H

#include "FtpsTypes.h"
#include <stdint.h>
#include <stddef.h>

/// Normalize a fingerprint string (strip separators, uppercase) into outBuf.
/// outBuf must be at least 65 bytes (64 hex chars + null).
/// Returns true if the input was a valid SHA-256 fingerprint.
bool ftpsTrustNormalizeFingerprint(const char *input, char *outBuf, size_t outBufLen);

/// Compare two normalized SHA-256 fingerprints (constant-time).
/// Both must be 64-char uppercase hex strings.
bool ftpsTrustFingerprintsMatch(const char *a, const char *b);

/// Validate that a PEM string looks structurally correct
/// (begins/ends with proper markers, within maxLen).
bool ftpsTrustValidatePem(const char *pem, size_t maxLen);

#endif // FTPS_TRUST_H

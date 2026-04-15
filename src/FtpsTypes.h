// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0

#ifndef FTPS_TYPES_H
#define FTPS_TYPES_H

#include <stdint.h>

enum class FtpsSecurityMode : uint8_t {
  Plain = 0,
  ExplicitTls = 1,
  ImplicitTls = 2,
};

enum class FtpsTrustMode : uint8_t {
  Fingerprint = 0,
  ImportedCert = 1,
};

struct FtpsServerConfig {
  const char *host = nullptr;
  uint16_t port = 21;
  const char *user = nullptr;
  const char *password = nullptr;
  const char *tlsServerName = nullptr;
  FtpsSecurityMode securityMode = FtpsSecurityMode::ExplicitTls;
  FtpsTrustMode trustMode = FtpsTrustMode::Fingerprint;
  const char *fingerprint = nullptr;
  const char *rootCaPem = nullptr;
  bool validateServerCert = true;
  bool passiveMode = true;
};

#endif // FTPS_TYPES_H

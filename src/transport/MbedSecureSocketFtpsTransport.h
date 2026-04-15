// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0
//
// Mbed TLSSocketWrapper-based FTPS transport — stub pending Phase 0 spike.

#ifndef MBED_SECURE_SOCKET_FTPS_TRANSPORT_H
#define MBED_SECURE_SOCKET_FTPS_TRANSPORT_H

#include "IFtpsTransport.h"

class MbedSecureSocketFtpsTransport : public IFtpsTransport {
public:
  bool connectControl(const FtpEndpoint &ep, const FtpTlsConfig &tls, char *error, size_t errorSize) override;
  bool upgradeControlToTls(const FtpTlsConfig &tls, char *error, size_t errorSize) override;

  int ctrlRead(uint8_t *buf, size_t len) override;
  int ctrlWrite(const uint8_t *buf, size_t len) override;
  bool ctrlConnected() override;

  bool openProtectedDataChannel(const FtpEndpoint &ep, const FtpTlsConfig &tls, char *error, size_t errorSize) override;
  int dataRead(uint8_t *buf, size_t len) override;
  int dataWrite(const uint8_t *buf, size_t len) override;
  bool dataConnected() override;
  void closeData() override;

  void closeAll() override;
};

#endif // MBED_SECURE_SOCKET_FTPS_TRANSPORT_H

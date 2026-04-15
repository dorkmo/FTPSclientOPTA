// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0
//
// Stub implementation — pending Phase 0 spike results.

#include "FtpsClient.h"
#include "transport/IFtpsTransport.h"
#include "transport/MbedSecureSocketFtpsTransport.h"

FtpsClient::FtpsClient() = default;

FtpsClient::~FtpsClient() {
  delete _transport;
}

bool FtpsClient::begin(NetworkInterface *network, char *error, size_t errorSize) {
  // TODO: create MbedSecureSocketFtpsTransport using network
  (void)network; (void)error; (void)errorSize;
  return false;
}

bool FtpsClient::connect(const FtpsServerConfig &config, char *error, size_t errorSize) {
  (void)config; (void)error; (void)errorSize;
  return false;
}

bool FtpsClient::store(const char *remotePath, const uint8_t *data, size_t length,
                       char *error, size_t errorSize) {
  (void)remotePath; (void)data; (void)length; (void)error; (void)errorSize;
  return false;
}

bool FtpsClient::retrieve(const char *remotePath, uint8_t *buffer, size_t bufferSize,
                          size_t &bytesRead, char *error, size_t errorSize) {
  (void)remotePath; (void)buffer; (void)bufferSize; (void)bytesRead; (void)error; (void)errorSize;
  return false;
}

void FtpsClient::quit() {
  if (_transport) {
    _transport->closeAll();
  }
}

FtpsError FtpsClient::lastError() const {
  return _lastError;
}

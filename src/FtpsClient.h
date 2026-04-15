// ArduinoOPTA-FTPS - Explicit FTPS client library for Arduino Opta
// SPDX-License-Identifier: CC0-1.0
//
// Stub header — implementation pending Phase 0 spike results.

#ifndef FTPS_CLIENT_H
#define FTPS_CLIENT_H

#include "FtpsTypes.h"
#include "FtpsErrors.h"

class IFtpsTransport;
class NetworkInterface;

/// Arduino-style FTPS client.
/// Owns its transport internally; callers only interact with this class.
class FtpsClient {
public:
  FtpsClient();
  ~FtpsClient();

  // Non-copyable — owns transport resources.
  FtpsClient(const FtpsClient &) = delete;
  FtpsClient &operator=(const FtpsClient &) = delete;

  /// Initialize the transport layer with a Mbed NetworkInterface.
  /// Call once before connect().  Returns false on failure.
  bool begin(NetworkInterface *network, char *error, size_t errorSize);

  /// Connect to the FTPS server described by config.
  bool connect(const FtpsServerConfig &config, char *error, size_t errorSize);

  /// Upload data to remotePath via STOR.
  bool store(const char *remotePath, const uint8_t *data, size_t length,
             char *error, size_t errorSize);

  /// Download remotePath via RETR into buffer.
  bool retrieve(const char *remotePath, uint8_t *buffer, size_t bufferSize,
                size_t &bytesRead, char *error, size_t errorSize);

  /// Send QUIT and close all sockets.
  void quit();

  /// Return the error code from the last failed operation.
  FtpsError lastError() const;

private:
  IFtpsTransport *_transport = nullptr;
  FtpsError _lastError = FtpsError::None;
};

#endif // FTPS_CLIENT_H

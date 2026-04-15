// ArduinoOPTA-FTPS - Basic Upload Example
// SPDX-License-Identifier: CC0-1.0
//
// Demonstrates uploading a file to an FTPS server using explicit TLS.
// Requires: Arduino Opta + Ethernet connection + FTPS server on the LAN.

#include <PortentaEthernet.h>
#include <Ethernet.h>
#include <FtpsClient.h>

static const char *FTP_HOST = "192.168.1.100";
static const uint16_t FTP_PORT = 21;
static const char *FTP_USER = "ftpuser";
static const char *FTP_PASS = "ftppass";

// SHA-256 fingerprint of the server's leaf certificate (64 hex chars, no separators)
static const char *SERVER_FINGERPRINT = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

void setup() {
  Serial.begin(115200);
  while (!Serial) { ; }

  Serial.println("Initializing Ethernet...");
  Ethernet.begin();

  FtpsClient ftps;
  char error[128] = {};

  if (!ftps.begin(Ethernet.getNetwork(), error, sizeof(error))) {
    Serial.print("Transport init failed: ");
    Serial.println(error);
    return;
  }

  FtpsServerConfig config;
  config.host = FTP_HOST;
  config.port = FTP_PORT;
  config.user = FTP_USER;
  config.password = FTP_PASS;
  config.trustMode = FtpsTrustMode::Fingerprint;
  config.fingerprint = SERVER_FINGERPRINT;
  config.validateServerCert = true;

  if (!ftps.connect(config, error, sizeof(error))) {
    Serial.print("Connect failed: ");
    Serial.println(error);
    return;
  }

  const char *data = "Hello from Arduino Opta!";
  if (!ftps.store("/upload/test.txt", (const uint8_t *)data, strlen(data), error, sizeof(error))) {
    Serial.print("Upload failed: ");
    Serial.println(error);
  } else {
    Serial.println("Upload succeeded.");
  }

  ftps.quit();
}

void loop() {
  // Nothing to do after setup.
}

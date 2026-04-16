"""Generate self-signed TLS certificate for pyftpdlib FTPS testing."""
from OpenSSL import crypto
import hashlib, os

key = crypto.PKey()
key.generate_key(crypto.TYPE_RSA, 2048)

cert = crypto.X509()
cert.get_subject().CN = "ftps-test-server"
cert.get_subject().O = "ArduinoOPTA-FTPS Test"
cert.set_serial_number(1000)
cert.gmtime_adj_notBefore(0)
cert.gmtime_adj_notAfter(365 * 24 * 60 * 60)
cert.set_issuer(cert.get_subject())
cert.set_pubkey(key)
cert.sign(key, "sha256")

base = os.path.dirname(os.path.abspath(__file__))
cert_path = os.path.join(base, "server.crt")
key_path = os.path.join(base, "server.key")

with open(cert_path, "wb") as f:
    f.write(crypto.dump_certificate(crypto.FILETYPE_PEM, cert))
with open(key_path, "wb") as f:
    f.write(crypto.dump_privatekey(crypto.FILETYPE_PEM, key))

der = crypto.dump_certificate(crypto.FILETYPE_ASN1, cert)
fp = hashlib.sha256(der).hexdigest().upper()
colon_fp = ":".join(fp[i:i+2] for i in range(0, len(fp), 2))

print(f"Certificate: {cert_path}")
print(f"Key:         {key_path}")
print(f"SHA-256:     {fp}")
print(f"SHA-256:     {colon_fp}")
print()
print("Paste the SHA-256 fingerprint (no colons) into FTP_FINGERPRINT in PyftpdlibLiveTest.ino")

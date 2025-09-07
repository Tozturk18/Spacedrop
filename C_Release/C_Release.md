# Spacedrop
Spacedrop: AirDrop without borders—share files &amp; links securely over Tailscale VPN.

# one-time
cd C_Release
make submodule          # fetch civetweb v1.16 into C_Release/third_party/civetweb
make build              # build hello (TLS/Zlib OFF by default)
make run                # launch http://localhost:8080

# optional features:
make build TLS=ON ZLIB=ON   # requires OpenSSL or MbedTLS + zlib installed

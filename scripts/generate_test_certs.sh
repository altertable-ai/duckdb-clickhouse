#!/usr/bin/env bash
# Generates a throw-away CA and a localhost server certificate for the ClickHouse test server.
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)/certs"
mkdir -p "$DIR"
cd "$DIR"
if [ -f server.crt ] && [ -f ca.crt ]; then
  exit 0
fi
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=clickhouse-scanner-test-ca" \
  -keyout ca.key -out ca.crt
openssl req -newkey rsa:2048 -nodes -subj "/CN=localhost" -keyout server.key -out server.csr
printf "subjectAltName=DNS:localhost,IP:127.0.0.1\n" > server.ext
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 3650 -sha256 \
  -extfile server.ext -out server.crt
# the server runs as uid 101 inside the container and must be able to read the key
chmod 644 server.key ca.key
rm -f server.csr server.ext ca.srl

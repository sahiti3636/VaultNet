#!/usr/bin/env bash
# gen_cert.sh – generate a self-signed TLS certificate for VaultNet development.
# Output: vaultnet.crt (PEM certificate) + vaultnet.key (PEM private key)
set -euo pipefail
umask 077

CERT="vaultnet.crt"
KEY="vaultnet.key"
DAYS=3650   # ~10 years

if [[ -f "$CERT" && -f "$KEY" ]]; then
    echo "Cert already exists ($CERT / $KEY). Delete them to regenerate."
    exit 0
fi
if [[ -e "$CERT" || -e "$KEY" ]]; then
    echo "Found only one of $CERT / $KEY; move it aside before regenerating." >&2
    exit 1
fi

openssl req -x509 -newkey rsa:2048 -keyout "$KEY" -out "$CERT" \
    -sha256 -days "$DAYS" -nodes \
    -subj "/CN=vaultnet-dev/O=VaultNet/C=US" \
    -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" \
    -addext "extendedKeyUsage=serverAuth" \
    2>/dev/null

echo "Generated: $CERT  $KEY  (self-signed, valid ${DAYS} days)"

#!/bin/sh
# gen-dev-keys.sh — regenerate the in-tree DEV RAUC signing keypair.
#
# The committed dev-ca.cert.pem / dev-ca.key.pem let a local A/B build sign and
# trust its own .raucb out-of-the-box. They are DEV-ONLY — never ship them to a
# production fleet. Production bundles are signed in CI from a secret key
# (RAUC_SIGNING_KEY) and the production cert is baked into the release image.
#
# Run from this directory to regenerate (e.g. after expiry):
#   ./gen-dev-keys.sh
set -eu
cd "$(dirname "$0")"
openssl req -x509 -newkey rsa:4096 -nodes \
    -keyout dev-ca.key.pem \
    -out    dev-ca.cert.pem \
    -subj "/O=iot-project/CN=iot-rauc-dev" \
    -days 3650
echo "regenerated dev-ca.{cert,key}.pem (DEV ONLY)"

#!/bin/sh
# Cloud image entrypoint.
#
# For the HTTPS iot-httpd, self-provision a self-signed TLS cert using the
# image's own openssl if one isn't already present at the configured path.
# This makes HTTPS fully self-contained: no host openssl, no run.sh cert
# step, no mounted cert — just a writable volume at the cert's directory.
# Every other service (ds-server, iot-cloudd, lwm2m-*) passes straight
# through untouched.
#
# Host the cert should be valid for: TLS_HOST env (run.sh passes the host's
# public IP). For a self-signed cert the SAN is cosmetic anyway (browsers
# warn regardless), but we set it correctly when we know it.
set -e

is_httpd=0
scheme=""
cert=""
key=""
for a in "$@"; do
  case "$a" in
    iot-httpd)     is_httpd=1 ;;
    http-scheme=*) scheme="${a#http-scheme=}" ;;
    http-cert=*)   cert="${a#http-cert=}" ;;
    http-key=*)    key="${a#http-key=}" ;;
  esac
done

if [ "$is_httpd" = "1" ] && [ "$scheme" = "https" ] && [ -n "$cert" ] && [ -n "$key" ] \
   && { [ ! -s "$cert" ] || [ ! -s "$key" ]; }; then
  host="${TLS_HOST:-$(hostname -i 2>/dev/null | awk '{print $1}')}"
  host="${host:-localhost}"
  case "$host" in
    *[!0-9.]*) san="DNS:$host" ;;   # has a non-digit/dot → treat as hostname
    *)         san="IP:$host" ;;
  esac
  mkdir -p "$(dirname "$cert")" "$(dirname "$key")"
  echo "iot-entrypoint: generating self-signed TLS cert (CN=$host, SAN=$san) -> $cert"
  openssl req -x509 -newkey rsa:2048 -nodes -days 825 \
    -keyout "$key" -out "$cert" -subj "/CN=$host" -addext "subjectAltName=$san"
fi

exec "$@"

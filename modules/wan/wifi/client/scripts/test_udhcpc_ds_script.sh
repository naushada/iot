#!/bin/sh
# Host test for udhcpc-ds.script: stub ds-cli + lease env, assert the emitted
# `ds-cli set` calls for bound/deconfig. No real udhcpc/ds-server needed.
#
# Usage: sh test_udhcpc_ds_script.sh   (exit 0 = pass)
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
hook="$here/udhcpc-ds.script"

work="${TMPDIR:-/tmp}/udhcpc-ds-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

# Stub ds-cli: log "key=value" per invocation (args: --socket=.. set KEY VALUE).
cat > "$work/ds-cli" <<'EOF'
#!/bin/sh
key=""; val=""
for a in "$@"; do
    case "$a" in --socket=*|set) : ;; *) if [ -z "$key" ]; then key="$a"; else val="$a"; fi ;; esac
done
printf '%s=%s\n' "$key" "$val" >> "$DSLOG"
EOF
chmod +x "$work/ds-cli"

DSLOG="$work/calls.log"; export DSLOG
export IOT_DS_CLI="$work/ds-cli"

fail() { echo "FAIL: $1"; exit 1; }
grep_q() { grep -qF "$1" "$DSLOG" || fail "expected '$1' in ds calls:\n$(cat "$DSLOG")"; }

# ── bound ──
: > "$DSLOG"
ip=192.168.1.42 subnet=255.255.255.0 router=192.168.1.1 \
dns="192.168.1.1 8.8.8.8" domain=lan lease=86400 \
    sh "$hook" bound
grep_q 'wifi.dhcp.ip="192.168.1.42"'
grep_q 'wifi.dhcp.mask="255.255.255.0"'
grep_q 'wifi.dhcp.gateway="192.168.1.1"'
grep_q 'wifi.dhcp.dns="192.168.1.1 8.8.8.8"'
grep_q 'wifi.dhcp.domain="lan"'
grep_q 'wifi.dhcp.lease.sec=86400'
grep_q 'wifi.dhcp.state="bound"'
grep -q 'wifi.dhcp.obtained.unix=' "$DSLOG" || fail "obtained.unix not set"

# ── deconfig clears data, does NOT touch state ──
: > "$DSLOG"
sh "$hook" deconfig
grep_q 'wifi.dhcp.ip=""'
grep_q 'wifi.dhcp.lease.sec=0'
grep -q 'wifi.dhcp.state=' "$DSLOG" && fail "deconfig must not write wifi.dhcp.state"

# ── quote-escaping in SSID-ish values (defensive) ──
: > "$DSLOG"
ip='1.2.3.4' subnet='' router='' dns='' domain='a"b\c' lease=10 sh "$hook" renew
grep_q 'wifi.dhcp.domain="a\"b\\c"'

# ── leasefail: no data writes ──
: > "$DSLOG"
sh "$hook" leasefail
[ -s "$DSLOG" ] && fail "leasefail must not write ds keys: $(cat "$DSLOG")"

echo "PASS: udhcpc-ds.script ($(basename "$hook"))"

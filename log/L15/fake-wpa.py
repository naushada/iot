#!/usr/bin/env python3
"""L15/D8 — fake wpa_supplicant for the wifi-client smoke harness.

Binds a unix-DGRAM socket at <ctrl_dir>/<iface>, accepts the subset
of wpa_supplicant's control commands the wifi-client daemon issues
(ATTACH, PING, SCAN, SCAN_RESULTS, ADD_NETWORK, SET_NETWORK,
ENABLE_NETWORK, SELECT_NETWORK, STATUS, LIST_NETWORKS, RECONFIGURE,
DISCONNECT, RECONNECT, TERMINATE), and emits canned CTRL-EVENT
notifications so the daemon walks through scanning -> connected.

Usage:
  fake-wpa.py --iface wlan0 --ctrl-dir /run/wpa_supplicant

The script terminates on SIGTERM/SIGINT or after `--once` (used by
smoke runs that want a deterministic exit).
"""

import argparse
import errno
import os
import signal
import socket
import sys
import time
from pathlib import Path


# Canned SCAN_RESULTS payload — TSV with header, two BSSIDs visible.
# The wifi-client smoke seeds wifi.networks=[{ssid:"HomeAP",...}],
# which matches the first SSID below so the daemon picks it.
CANNED_SCAN = (
    "bssid / frequency / signal level / flags / ssid\n"
    "aa:bb:cc:dd:ee:ff\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\tHomeAP\n"
    "11:22:33:44:55:66\t2417\t-67\t[WPA2-PSK-CCMP][ESS]\tGuestNet\n"
)


def parse_args():
    """Accept both our own --long-form flags AND wpa_supplicant's
    single-dash argv shape (-i <iface> -c <conf> -C <ctrl-dir>
    [-D <driver_list>]) so wifi-client can run us in place of the
    real binary via `--wpa=log/L15/fake-wpa.py`.
    """
    argv = sys.argv[1:]
    iface     = "wlan0"
    ctrl_dir  = "/run/wpa_supplicant"
    once      = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if   a == "--iface"    and i + 1 < len(argv): iface    = argv[i+1]; i += 2
        elif a == "--ctrl-dir" and i + 1 < len(argv): ctrl_dir = argv[i+1]; i += 2
        elif a == "--once":                           once     = True;       i += 1
        elif a == "-i"          and i + 1 < len(argv): iface    = argv[i+1]; i += 2
        elif a == "-C"          and i + 1 < len(argv): ctrl_dir = argv[i+1]; i += 2
        # Ignore wpa_supplicant args we don't care about.
        elif a == "-c"          and i + 1 < len(argv): i += 2   # config file
        elif a == "-D"          and i + 1 < len(argv): i += 2   # driver list
        elif a in ("-B", "-f", "-W", "-K"):           i += 1
        else:
            sys.stderr.write(f"[fake-wpa] ignoring arg: {a}\n")
            i += 1
    class A: pass
    rv = A()
    rv.iface, rv.ctrl_dir, rv.once = iface, ctrl_dir, once
    return rv


def emit(sock, peer, line):
    """Send `line` (with trailing newline) to peer as an event."""
    if not peer:
        return
    if not line.endswith("\n"):
        line += "\n"
    try:
        sock.sendto(line.encode(), peer)
    except OSError as e:
        # peer may have closed its socket file (test ends);
        # silent drop is fine.
        sys.stderr.write(f"[fake-wpa] event send dropped: {e}\n")


def main():
    args = parse_args()
    sock_path = Path(args.ctrl_dir) / args.iface
    sock_path.parent.mkdir(parents=True, exist_ok=True)
    if sock_path.exists():
        sock_path.unlink()

    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    s.bind(str(sock_path))
    os.chmod(str(sock_path), 0o666)

    print(f"[fake-wpa] listening at {sock_path}", file=sys.stderr)

    # Track the attached peer so unsolicited events can be addressed.
    attached_peer = None
    done = {"flag": False}

    def shutdown(*_):
        done["flag"] = True
    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT,  shutdown)

    s.settimeout(0.5)
    while not done["flag"]:
        try:
            data, peer = s.recvfrom(4096)
        except socket.timeout:
            continue
        except OSError as e:
            if e.errno == errno.EINTR:
                continue
            raise
        cmd = data.decode(errors="replace").rstrip("\r\n")
        # Single-word commands first, then prefix matches.
        head = cmd.split(" ", 1)[0].upper()
        if head == "PING":
            s.sendto(b"PONG\n", peer)
        elif head == "ATTACH":
            attached_peer = peer
            s.sendto(b"OK\n", peer)
            print(f"[fake-wpa] ATTACH from {peer}", file=sys.stderr)
        elif head == "DETACH":
            attached_peer = None
            s.sendto(b"OK\n", peer)
        elif head == "SCAN":
            s.sendto(b"OK\n", peer)
            # Walk through scan -> select -> connected so the
            # supervisor's lifecycle FSM advances even without us
            # waiting for explicit SELECT_NETWORK from the daemon.
            time.sleep(0.05)
            emit(s, attached_peer, "<3>CTRL-EVENT-SCAN-STARTED")
            time.sleep(0.05)
            emit(s, attached_peer, "<3>CTRL-EVENT-SCAN-RESULTS")
            time.sleep(0.05)
            emit(s, attached_peer,
                 "<3>CTRL-EVENT-CONNECTED - Connection to "
                 "aa:bb:cc:dd:ee:ff completed [id=0 id_str=HomeAP]")
            if args.once:
                # Linger briefly so the daemon's --once branch can
                # observe the CONNECTED event before we exit.
                time.sleep(0.3)
                done["flag"] = True
        elif head == "SCAN_RESULTS":
            s.sendto(CANNED_SCAN.encode(), peer)
        elif head in {"ADD_NETWORK"}:
            # wpa_supplicant returns the new network id (we always
            # return 0 — single network in our fake universe).
            s.sendto(b"0\n", peer)
        elif head in {"SET_NETWORK", "ENABLE_NETWORK", "SELECT_NETWORK",
                      "RECONFIGURE", "RECONNECT", "DISCONNECT", "SAVE_CONFIG",
                      "REMOVE_NETWORK", "DISABLE_NETWORK", "TERMINATE",
                      "QUIT"}:
            s.sendto(b"OK\n", peer)
            if head == "TERMINATE" or head == "QUIT":
                emit(s, attached_peer, "<3>CTRL-EVENT-TERMINATING")
                done["flag"] = True
        elif head == "LIST_NETWORKS":
            s.sendto(b"network id / ssid / bssid / flags\n"
                     b"0\tHomeAP\tany\t[CURRENT]\n",
                     peer)
        elif head == "STATUS":
            s.sendto(
                b"bssid=aa:bb:cc:dd:ee:ff\n"
                b"ssid=HomeAP\n"
                b"id=0\n"
                b"wpa_state=COMPLETED\n"
                b"key_mgmt=WPA2-PSK\n"
                b"address=02:00:00:00:00:01\n",
                peer)
        elif head == "SIGNAL_POLL":
            s.sendto(b"RSSI=-42\nLINKSPEED=72\nNOISE=-95\nFREQUENCY=2412\n",
                     peer)
        else:
            # Default: FAIL so the supervisor surfaces it as an
            # unknown command path rather than hanging on recv.
            s.sendto(b"FAIL\n", peer)

    print("[fake-wpa] exiting", file=sys.stderr)
    try:
        sock_path.unlink()
    except OSError:
        pass


if __name__ == "__main__":
    main()

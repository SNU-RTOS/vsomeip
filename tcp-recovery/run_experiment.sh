#!/bin/bash
# End-to-end SOME/IP TCP packet-recovery experiment: runs the server locally, drives the
# client over SSH on a remote host, injects wire loss on this host for the duration, and
# captures the server-side flow for tcp-recovery/analyze_recovery.py.
#
# This is a convenience wrapper around the three standalone pieces (inject_loss.sh,
# vsomeip itself, analyze_recovery.py) for the common two-board setup. For anything else
# (client on this host too, no SSH available, a different loss pattern), run those three
# pieces by hand instead - see tcp-recovery/README.md.
#
# Usage:
#   sudo ./run_experiment.sh <label> <mode: rr|stream> <cycle_ms> <duration_s> \
#        --server-ip IP --client-ip IP --client-host user@host [options]
#
# Required:
#   --server-ip IP        this host's vsomeip unicast address (must match
#                          config/vsomeip-tcp-service.json's "unicast")
#   --client-ip IP        the client's vsomeip unicast address (must match its
#                          config/vsomeip-tcp-client.json's "unicast")
#   --client-host H       ssh destination for the client host, e.g. user@192.168.1.5
#
# Client authentication - never pass a password on the command line or hardcode one in
# a script that might get committed. Either:
#   - set up an SSH key beforehand (recommended):
#       ssh-copy-id <client-host>
#   - or export SSHPASS in your shell before running this script (not in any file):
#       export SSHPASS='...'; sudo -E ./run_experiment.sh ...
#     (sudo -E preserves it into the root shell this script needs for iptables/ip route;
#     requires the `sshpass` package)
#
# Options:
#   --config-dir DIR       where tcp-*.json live               (default: ../config,
#                           relative to this script)
#   --vsomeip-dir DIR       vsomeip repo root on the CLIENT host (default: same path as
#                           --client-remote-dir if given, else /workspace/vsomeip)
#   --client-remote-dir DIR  alias for --vsomeip-dir
#   --client-sudo           run the client binary via `sudo -S` on the remote host
#                           (needed if its vsomeip checkout is root-owned)
#   --port PORT             reliable SOME/IP TCP port                 (default: 30510)
#   --every N               drop 1-in-N first transmissions           (default: 8)
#   --no-drop               run without loss injection (baseline / sanity check)
#   --out DIR               where to write results                    (default:
#                           ./results/<label>)
set -e
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
V=$(cd "$SCRIPT_DIR/.." && pwd)
T=/sys/kernel/tracing

LABEL=$1; MODE=$2; CYCLE=$3; DUR=$4; shift 4 || true
CONFIG_DIR="$V/config"; VSOMEIP_DIR=""; CLIENT_SUDO=0; PORT=30510; EVERY=8; DROP=1; OUT=""
SERVER_IP=""; CLIENT_IP=""; CLIENT_HOST=""

while [ $# -gt 0 ]; do
    case "$1" in
        --server-ip) SERVER_IP=$2; shift 2 ;;
        --client-ip) CLIENT_IP=$2; shift 2 ;;
        --client-host) CLIENT_HOST=$2; shift 2 ;;
        --config-dir) CONFIG_DIR=$2; shift 2 ;;
        --vsomeip-dir|--client-remote-dir) VSOMEIP_DIR=$2; shift 2 ;;
        --client-sudo) CLIENT_SUDO=1; shift ;;
        --port) PORT=$2; shift 2 ;;
        --every) EVERY=$2; shift 2 ;;
        --no-drop) DROP=0; shift ;;
        --out) OUT=$2; shift 2 ;;
        *) echo "알 수 없는 옵션: $1" >&2; exit 1 ;;
    esac
done
VSOMEIP_DIR=${VSOMEIP_DIR:-/workspace/vsomeip}
OUT=${OUT:-$SCRIPT_DIR/results/$LABEL}

[ -n "$LABEL" ] && [ -n "$MODE" ] && [ -n "$CYCLE" ] && [ -n "$DUR" ] || {
    echo "usage: sudo $0 <label> <rr|stream> <cycle_ms> <duration_s> --server-ip IP --client-ip IP --client-host H [options]" >&2
    exit 1
}
[ -n "$SERVER_IP" ] && [ -n "$CLIENT_IP" ] && [ -n "$CLIENT_HOST" ] || {
    echo "--server-ip, --client-ip, --client-host는 필수" >&2; exit 1
}
[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행하세요 (iptables/ip route/tcpdump 필요)" >&2; exit 1; }
[ "$MODE" = rr ] || [ "$MODE" = stream ] || { echo "mode는 rr 또는 stream" >&2; exit 1; }

if [ -n "$SSHPASS" ]; then
    command -v sshpass >/dev/null || { echo "SSHPASS가 설정됐지만 sshpass가 설치되어 있지 않음" >&2; exit 1; }
    SSH="sshpass -e ssh -o StrictHostKeyChecking=no -o LogLevel=ERROR $CLIENT_HOST"
else
    SSH="ssh -o LogLevel=ERROR $CLIENT_HOST"
    timeout 5 $SSH true 2>/dev/null || {
        echo "키 기반 SSH 접속 실패. ssh-copy-id로 키를 등록하거나 SSHPASS를 export 하세요." >&2
        exit 1
    }
fi
FEED_PW=0
if [ "$CLIENT_SUDO" = 1 ]; then
    if [ -n "$SSHPASS" ]; then
        # Feed the password to the REMOTE sudo prompt through the ssh session's own
        # stdin pipe (below), not as literal text in the command string - sshpass only
        # intercepts the initial SSH login prompt via its own pty, so this is a second,
        # separate hand-off, but it means the password never appears in any argv (so it
        # never shows up in `ps` output on either host) and is never written to a file.
        CLIENT_PREFIX='sudo -S -p ""'
        FEED_PW=1
    else
        # No SSHPASS: assume key-based login and passwordless (NOPASSWD) sudo on the
        # remote host for this command. If that is not set up, the run will fail with
        # a permission error - configure sudoers or export SSHPASS instead.
        CLIENT_PREFIX='sudo -n'
    fi
else
    CLIENT_PREFIX=''
fi
run_remote() {  # run_remote <remote-command-string>
    if [ "$FEED_PW" = 1 ]; then echo "$SSHPASS" | $SSH "$1"
    else $SSH "$1"
    fi
}

mkdir -p "$OUT"; rm -f "$OUT"/*
# Derive the interface from the actual route to the client, not the default route - on a
# multi-homed host (e.g. both wired and Wi-Fi on the same subnet as the client) those can
# differ, and capturing/injecting loss on the wrong one silently sees no matching traffic.
IF=$(ip route get "$CLIENT_IP" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')
IF=${IF:-$(ip route show default | awk '/default/{print $5; exit}')}

if [ "$MODE" = rr ]; then
    SRV_BIN="build/examples/response-sample"
    CLI_CMD="build/examples/request-sample --tcp --cycle $CYCLE"
    IPLEN="150:1500"
else
    SRV_BIN="build/examples/notify-sample --cycle $CYCLE"
    CLI_CMD="build/examples/subscribe-sample --tcp"
    IPLEN="69:1500"
fi

cleanup() {
    [ "$DROP" = 1 ] && IFACE="$IF" PORT="$PORT" EVERY="$EVERY" IPLEN="$IPLEN" \
        "$SCRIPT_DIR/inject_loss.sh" stop "$SERVER_IP" "$CLIENT_IP" >/dev/null 2>&1
    for p in $(pgrep -x tcpdump); do kill "$p" 2>/dev/null; done
    # kill -9 on an already-exited PID (the common case: the server took the plain kill
    # fine) fails - and being the last command of this { } group, under `set -e` that
    # failure is NOT exempted the way a plain `&&` chain member would be, so it would
    # abort this trap handler right here without `|| true`.
    [ -n "$SVC" ] && { kill "$SVC" 2>/dev/null; sleep 1; kill -9 "$SVC" 2>/dev/null || true; }
    # pkill -f, not -x, for subscribe-sample: at 16 characters it exceeds the 15-char comm-name
    # limit pgrep/pkill -x matches against, so -x would silently match nothing at all.
    run_remote "$CLIENT_PREFIX bash -c 'cd $VSOMEIP_DIR && pkill -f build/examples/subscribe-sample; pkill -x request-sample; rm -f /tmp/vsomeip-* /tmp/vsomeip.lck'" 2>/dev/null || true
}
trap cleanup EXIT

# push a client config carrying the right unicast onto the client host's /tmp - avoids
# needing to hand-edit the checked-in config on every run
python3 - "$CONFIG_DIR/vsomeip-tcp-client.json" "$CLIENT_IP" > "$OUT/tcp-cli.json" <<'PY'
import json, sys
c = json.load(open(sys.argv[1]))
c["unicast"] = sys.argv[2]
json.dump(c, sys.stdout, indent=4)
PY
$SSH "cat > /tmp/tcp-cli-runtime.json" < "$OUT/tcp-cli.json"

cd "$V"
# VSOMEIP_SD_FAST_START: required, not just an optimization, on a gateway-less direct link
# (e.g. two boards' Ethernet ports cabled straight together, no switch) - without it SD
# waits forever for a netlink "default route" event that such a link never produces. See
# response_sd.sh / CHANGES_THOR.md for the full story. Override with 0 if you have a normal
# routed network and want the original (slightly slower) startup path.
env LD_LIBRARY_PATH=build \
    VSOMEIP_CONFIGURATION="$CONFIG_DIR/vsomeip-tcp-service.json" \
    VSOMEIP_APPLICATION_NAME=service-sample \
    VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}" \
    $SRV_BIN > "$OUT/server.log" 2>&1 &
SVC=$!
sleep 3

if [ "$DROP" = 1 ]; then
    IFACE="$IF" PORT="$PORT" EVERY="$EVERY" IPLEN="$IPLEN" \
        "$SCRIPT_DIR/inject_loss.sh" start "$SERVER_IP" "$CLIENT_IP"
fi

tcpdump -i "$IF" -n -tt --time-stamp-precision=micro -w "$OUT/srv.pcap" "tcp port $PORT" >/dev/null 2>&1 &

# The client runs for the full duration and is then cut off by `timeout` - that is the
# intended, successful end of the run, not a failure, but a plain `timeout` (without
# --preserve-status) always reports 124 when it actually had to signal the process,
# regardless of how cleanly the process itself shut down. Do not let that trip `set -e`.
run_remote "$CLIENT_PREFIX bash -c 'cd $VSOMEIP_DIR && LD_LIBRARY_PATH=build VSOMEIP_CONFIGURATION=/tmp/tcp-cli-runtime.json VSOMEIP_APPLICATION_NAME=client-sample VSOMEIP_SD_FAST_START=1 timeout $DUR $CLI_CMD'" \
    > "$OUT/client.log" 2>&1 || true

for p in $(pgrep -x tcpdump); do kill "$p" 2>/dev/null; done; sleep 1
tcpdump -r "$OUT/srv.pcap" -n -S -tt --time-stamp-precision=micro > "$OUT/srv.txt" 2>/dev/null
echo "$LABEL mode=$MODE cycle=${CYCLE}ms dur=${DUR}s every=1/$EVERY drop=$DROP" > "$OUT/meta.txt"

echo
echo "=== $OUT/meta.txt ==="; cat "$OUT/meta.txt"
python3 "$SCRIPT_DIR/analyze_recovery.py" --capture "$OUT/srv.txt" --port "$PORT"

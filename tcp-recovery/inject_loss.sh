#!/bin/bash
# Wire-loss emulation for SOME/IP TCP packet-recovery testing.
#
# Marks the FIRST transmission of every 1-in-N TCP segments the server sends on the
# reliable SOME/IP port, and policy-routes marked packets into a dummy interface instead
# of the real one. A dummy device reports NETDEV_TX_OK, so the kernel's TCP stack sees a
# successful send and behaves exactly as if the segment vanished on the wire.
#
# Why not `iptables ... -j DROP`: an OUTPUT-chain DROP is visible to the sending host's own
# TCP stack as a *send error* (-EPERM). TCP treats that as "never sent" and silently re-queues
# the segment on the next write, which is not what a real link-layer loss looks like and does
# not exercise the retransmission timers under test. Measured effect of this mistake: 0 kernel
# retransmits recorded, full recovery deferred to the *application's* request retry.
#
# Why not `tc netem loss`: the sch_netem qdisc is not built on either Thor's or the Orin's
# kernel (`modinfo sch_netem` fails on both, as of 2026-09). Rebuilding the kernel module is
# out of scope for a test harness.
#
# Only every-Nth FIRST transmission is targeted (`-m statistic --mode nth --packet 0`), so
# retransmissions of an already-marked segment are never re-marked. Selecting on the SOME/IP
# session id (embedded at a fixed offset once the IP total length pins the payload size) keeps
# the loss reproducible instead of hitting a random byte in the stream.
#
# Run with sudo on the machine acting as TCP server for the flow under test (the sender whose
# retransmission behaviour you want to observe).
set -e

usage() {
    cat >&2 <<EOF
Usage:
  sudo $0 start <server-ip> <client-ip> [options]
  sudo $0 stop  <server-ip> <client-ip> [options]
  sudo $0 status

Options (env vars, same defaults for start/stop - pass the same ones to both):
  IFACE=<name>        egress interface (default: interface of the default route)
  PORT=<n>             reliable SOME/IP TCP port to target        (default: 30510)
  EVERY=<n>            drop 1 in every N first-transmissions       (default: 8)
  IPLEN=<min>:<max>    only match segments whose IP total length falls in this
                       range, i.e. the payload size band to target (default: 69:1500,
                       which is "any data segment" - raise the min to exclude small
                       control/ack-only segments if your service sends those on this
                       port too)
  MARK=<hex>           fwmark value used internally                (default: 0x5a)
  TABLE=<n>            policy-routing table id used internally      (default: 105)

Example (SOME/IP TCP server on this host, port 30510, client on the Orin):
  sudo $0 start 192.168.196.246 192.168.196.122
  ... run the test ...
  sudo $0 stop  192.168.196.246 192.168.196.122

Dropped segments are logged to the kernel log with the prefix "SOMEIP_DROP ", one line
per drop - use this to sanity-check the drop rate actually seen (dmesg's timestamp is
kernel uptime, not wall-clock, so it cannot be lined up with a tcpdump capture for
timing - it is a count, not a recovery-time source):
  sudo dmesg | grep -c SOMEIP_DROP
EOF
    exit 1
}

CMD=$1; SRV_IP=$2; CLI_IP=$3
[ "$CMD" = status ] || { [ -n "$SRV_IP" ] && [ -n "$CLI_IP" ]; } || usage
[ "$(id -u)" -eq 0 ] || { echo "run me with sudo" >&2; exit 1; }

# Derive the interface from the actual route TO THE PEER, not the default route - on a
# multi-homed host (e.g. both wired and Wi-Fi on the same subnet as the peer) those can
# differ, and capturing/injecting on the wrong one silently sees no matching traffic.
IFACE=${IFACE:-$(ip route get "$CLI_IP" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')}
IFACE=${IFACE:-$(ip route show default | awk '/default/{print $5; exit}')}
PORT=${PORT:-30510}
EVERY=${EVERY:-8}
IPLEN=${IPLEN:-69:1500}
MARK=${MARK:-0x5a}
TABLE=${TABLE:-105}
DUMMY=somloss0
CHAIN=SOMEIP_LOSS

# Match: TCP segments from $SRV_IP, source port $PORT, IP total length in $IPLEN,
# whose SOME/IP session id (2 bytes at TCP-payload offset 14, i.e. right after the
# SOME/IP header's message-id/length/client-id fields) is a multiple of $EVERY -
# checked via `-m statistic --mode nth --every $EVERY --packet 0`, which fires on
# every Nth packet *matching everything before it in the rule*, not on the session id
# itself. Restricting by IP length first keeps this aligned to one segment per
# SOME/IP message in the common case where each message is one TCP segment.
U32="0&0xFFFF=${IPLEN}&&0>>22&0x3C@12>>26&0x3C@8&0x7=0"
MATCH=(-p tcp -s "$SRV_IP" --sport "$PORT" -m u32 --u32 "$U32" -m statistic --mode nth --every "$EVERY" --packet 0)

case "$CMD" in
  start)
    ip link show "$DUMMY" >/dev/null 2>&1 || ip link add "$DUMMY" type dummy
    ip link set "$DUMMY" up
    ip route replace "$CLI_IP"/32 dev "$DUMMY" table "$TABLE"
    ip rule add fwmark "$MARK" lookup "$TABLE" priority "$TABLE" 2>/dev/null || true
    iptables -t mangle -N "$CHAIN" 2>/dev/null || true
    iptables -t mangle -F "$CHAIN"
    iptables -t mangle -A "$CHAIN" -j LOG --log-prefix "SOMEIP_DROP "
    iptables -t mangle -A "$CHAIN" -j MARK --set-mark "$MARK"
    iptables -t mangle -I OUTPUT "${MATCH[@]}" -j "$CHAIN" || {
        echo "규칙 삽입 실패 - u32/statistic 모듈이 없는지 확인 (lsmod | grep xt_u32)" >&2
        exit 1
    }
    echo "적용됨: $IFACE 상의 $SRV_IP:$PORT -> $CLI_IP, 1/$EVERY 유실, 길이 $IPLEN"
    ;;
  stop)
    iptables -t mangle -D OUTPUT "${MATCH[@]}" -j "$CHAIN" 2>/dev/null || true
    iptables -t mangle -F "$CHAIN" 2>/dev/null || true
    iptables -t mangle -X "$CHAIN" 2>/dev/null || true
    ip rule del fwmark "$MARK" lookup "$TABLE" priority "$TABLE" 2>/dev/null || true
    ip route flush table "$TABLE" 2>/dev/null || true
    ip link del "$DUMMY" 2>/dev/null || true
    echo "해제됨"
    ;;
  status)
    echo "mangle 규칙: $(iptables -t mangle -S 2>/dev/null | grep -c SOMEIP_LOSS)"
    echo "policy rule (fwmark ${MARK:-0x5a}): $(ip rule 2>/dev/null | grep -c "${MARK:-0x5a}")"
    echo "dummy 인터페이스: $(ip link show $DUMMY 2>/dev/null | grep -c $DUMMY)"
    ;;
  *) usage ;;
esac

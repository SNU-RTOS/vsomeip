#!/bin/bash
# TCP packet-recovery tuning for a SOME/IP TCP endpoint.
#
# Run with sudo on whichever machine acts as TCP server for the flow under test - it is
# the server that retransmits, so these knobs only affect its behaviour.
#
# Both settings are volatile (lost on reboot) and reversible - see the printed "되돌리기"
# line. Measured effect (Thor server -> Orin client over Wi-Fi, 5ms-cycle SOME/IP events,
# 1-in-8 first-transmissions lost via inject_loss.sh; [C] = client-perceived recovery time,
# see analyze_recovery.py):
#
#   default                          : [C] mean 6.51ms  p50 1.90ms  n=292
#   + rto_min 1ms                    : [C] mean 4.07ms  p50 2.11ms  n=273
#   + rto_min 1ms + tcp_reordering=1 : [C] mean 3.46ms  p50 1.64ms  n=293   <- this script
#
# See tcp-recovery/README.md for the full breakdown and for what still stands between
# 3.46ms and the 2ms target (mainly CONFIG_HZ=250 on both boards - not something this
# script can fix).
set -e

[ "$(id -u)" -eq 0 ] || { echo "run me with sudo" >&2; exit 1; }
[ -n "$1" ] || { echo "usage: sudo $0 <peer-ip> [interface]" >&2; exit 1; }
PEER=$1
# Derive the interface from the actual route to the peer, not the default route - on a
# multi-homed host (e.g. both wired and Wi-Fi on the same subnet as the peer) those can
# differ, and attaching rto_min to the wrong interface's route is a silent no-op for the
# path that matters. (Passing $2 explicitly always overrides this.)
IF=${2:-$(ip route get "$PEER" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')}
IF=${IF:-$(ip route show default | awk '/default/{print $5; exit}')}

# 1. Lower the minimum retransmission timeout for this one peer only (a per-route
#    setting, not global). The kernel default (tcp_rto_min, 200ms) is what a genuinely
#    timer-driven recovery costs when no later segment arrives to trigger a SACK-based
#    fast retransmit - e.g. the last segment of a burst, or lockstep request/response
#    traffic where nothing follows the lost reply. It does not speed up the common
#    SACK-triggered path (that path never reaches the RTO timer), only its floor.
ip route replace "$PEER"/32 dev "$IF" rto_min 1ms
echo "rto_min 1ms via $IF toward $PEER"

# 2. Lower the SACK reordering threshold (GLOBAL sysctl - affects every TCP connection
#    on this host, not just $PEER). RACK/SACK-based fast retransmit waits for
#    tcp_reordering (default 3) duplicate-ACK/SACK signals before declaring a segment
#    lost, specifically to tell real loss apart from packets that merely arrived
#    out of order. A link that does not reorder packets - a direct or near-direct
#    hop, as tested here - gets no benefit from that wait, only the latency.
#    Do not set this on a path with real reordering (e.g. multi-path Wi-Fi roaming,
#    aggregated links): spurious retransmits will follow.
sysctl -w net.ipv4.tcp_reordering=1

echo
echo "되돌리기:"
echo "  sudo ip route del $PEER/32 dev $IF"
echo "  sudo sysctl -w net.ipv4.tcp_reordering=3"

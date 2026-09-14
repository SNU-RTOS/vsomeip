#!/bin/bash
# Applies tune-latency.sh (host: cpuidle/governor/EEE, helps SD and TCP alike) and, if a peer
# IP is given, tcp-recovery/tune_tcp_recovery.sh (TCP-only: rto_min + tcp_reordering) in one
# shot.
#
# Run once with sudo after every reboot on each board - not before every individual test run.
# Both scripts set volatile host state (governor, cpuidle, EEE, a route's rto_min, a sysctl)
# that stays in effect until the next reboot, regardless of how many SD/TCP tests you run in
# between. There is no ordering requirement against request_sd.sh/response_sd.sh/
# request_tcp.sh/response_tcp.sh either - apply this whenever, then run tests however many
# times you like.
#
# 사용법: sudo ./tune-all.sh <피어 IP> [인터페이스]
#   예: sudo ./tune-all.sh 10.10.10.2      # Thor에서, Orin이 피어일 때
#
# 피어 IP는 SD만 테스트할 거여도 넣을 것. 이유: tune-latency.sh를 피어 IP 없이 단독으로
# 쓰면 "기본 라우트가 걸린 인터페이스"로 자동 감지하는데, 이 프로젝트의 이더넷 직결 링크
# (예: enP2p1s0/eno1)는 게이트웨이 없는 점대점 링크라 애초에 기본 라우트가 안 잡힌다 -
# SD도 이제 이 링크로 돈다. 그래서 피어 IP 없이 실행하면 실제로는 엉뚱한 인터페이스(보통
# Wi-Fi)가 튜닝된다. 피어 IP를 주면 "그 피어로 실제 가는 경로"(tcp-recovery/*.sh와 동일한
# 방식)로 인터페이스를 알아내 이 문제를 피해간다 - TCP 전용 튜닝(rto_min/tcp_reordering)이
# SD에 해가 되지는 않으므로, 매번 피어 IP를 주는 쪽이 안전하다.
#
# 피어 IP 없이 실행하면 tune-latency.sh만 기본 라우트 기준으로 적용된다 - 이 프로젝트가
# 아닌, 게이트웨이가 있는 일반 네트워크에서 쓸 때만 의미가 있다.
set -e
[ "$(id -u)" -eq 0 ] || { echo "run me with sudo" >&2; exit 1; }

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PEER=$1
IFACE=$2

if [ -n "$PEER" ] && [ -z "$IFACE" ]; then
    IFACE=$(ip route get "$PEER" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')
fi

echo "=== 1/2: host latency tuning (tune-latency.sh) ==="
"$SCRIPT_DIR/tune-latency.sh" ${IFACE:+"$IFACE"}

if [ -n "$PEER" ]; then
    echo
    echo "=== 2/2: TCP recovery tuning (tcp-recovery/tune_tcp_recovery.sh) ==="
    "$SCRIPT_DIR/tcp-recovery/tune_tcp_recovery.sh" "$PEER" ${IFACE:+"$IFACE"}
else
    echo
    echo "피어 IP를 안 줘서 tune_tcp_recovery.sh(rto_min/tcp_reordering)는 건너뜀."
    echo "TCP 패킷 복구 테스트도 할 거면: sudo $0 <피어 IP>"
fi

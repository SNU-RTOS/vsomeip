#!/bin/bash

# UDP "fast recovery" 프로토타입 - 서버. sudo로 실행(유실 주입에 iptables/ip route 필요).
# 짝: request_udp_recovery.sh (반대쪽 보드에서 실행).
#
# TCP 버전(response_tcp.sh)과 달리 tcpdump 기반 와이어 레벨 분석이 필요 없다 - 클라이언트가
# UDP 시퀀스 번호의 불연속을 직접 보고 유실을 판단하므로(휴리스틱 아님) 클라이언트 자신의
# 숫자가 이미 ground truth다. 그래서 이 스크립트는 유실 주입 + 서버 실행만 하고 조용히 떠
# 있다가, 클라이언트가 스스로 멈추면(요청 성공 건수에 도달하거나 Ctrl-C) SD 세션이 끊기는
# 걸로 자연스럽게 끝난다 - 직접 멈춰야 하면 Ctrl-C.
#
# 유실은 이벤트 포트(기본 30511)에만 주입된다 - 복구 RPC는 별도 포트(30512)라 주입 대상이
# 아니다(examples/response-udp-recovery.cpp 상단 주석 참고). 이게 바로 이 프로토타입의
# 핵심 - "유실 복구"를 커널 TCP 재전송이 아니라 우리가 만든 RPC가 대신하게 하는 것.
#
# 사용법: sudo ./response_udp_recovery.sh <클라이언트 IP> [주기ms=1] [1/N 유실=8]
set -e

[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행하세요 (유실 주입에 iptables/ip route가 필요)" >&2; exit 1; }
CLIENT_IP=$1
[ -n "$CLIENT_IP" ] || { echo "usage: sudo $0 <클라이언트 IP> [주기ms=1] [1/N 유실=8]" >&2; exit 1; }
CYCLE="${2:-1}"
EVERY="${3:-8}"
PORT="${PORT:-30511}"

SERVER_IP=$(python3 -c "import json; print(json.load(open('config/vsomeip-udp-recovery-service.json'))['unicast'])")
[ "$CLIENT_IP" != "$SERVER_IP" ] || {
    echo "<클라이언트 IP>에 이 보드 자신의 주소($SERVER_IP)를 줬다 - 상대 보드의 IP를 줘야 한다" >&2
    exit 1
}

# -f를 쓰는 이유는 response_tcp.sh와 같다: "response-udp-recovery"가 15자를 넘어
# pgrep -x(15자에서 잘림)로는 못 찾는다.
pgrep -f build/examples/response-udp-recovery >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

cleanup() {
    PROTO=udp PORT="$PORT" EVERY="$EVERY" ./tcp-recovery/inject_loss.sh stop "$SERVER_IP" "$CLIENT_IP" >/dev/null 2>&1 || true
}
trap cleanup EXIT

PROTO=udp PORT="$PORT" EVERY="$EVERY" ./tcp-recovery/inject_loss.sh start "$SERVER_IP" "$CLIENT_IP"

export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-udp-recovery-service.json"
export VSOMEIP_APPLICATION_NAME="service-sample"
# 게이트웨이 없는 직결 링크에서는 netlink의 "기본 라우트" 이벤트가 오지 않아 이게 없으면 SD
# 자체가 시작되지 않는다 (response_sd.sh 참고).
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

build/examples/response-udp-recovery --cycle "$CYCLE"

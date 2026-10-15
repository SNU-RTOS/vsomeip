#!/bin/bash

# 하이브리드 복구 테스트 - 서버. sudo로 실행(유실 주입에 iptables/ip route 필요).
# 짝: request_hybrid_recovery.sh (반대쪽 보드에서 실행).
#
# response_tcp.sh와 거의 같다 - TCP 이벤트 스트림(포트 30516)에 똑같이 유실을 주입한다.
# 다른 점은 서버가 UDP 전용 두 번째 인스턴스(포트 30517)도 같이 띄워서, 클라이언트가
# TCP 스트림이 멈췄다고 판단하는 즉시 그 쪽으로 "지금 값 뭐야" RPC를 보낼 수 있게 해준다는
# 것뿐 - 그 RPC 포트 자체는 유실 주입 대상이 아니다(examples/response-hybrid-recovery.cpp
# 상단 주석 참고).
#
# 사용법: sudo ./response_hybrid_recovery.sh <클라이언트 IP> [주기ms=1] [1/N 유실=8]
set -e

[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행하세요 (유실 주입에 iptables/ip route가 필요)" >&2; exit 1; }
CLIENT_IP=$1
[ -n "$CLIENT_IP" ] || { echo "usage: sudo $0 <클라이언트 IP> [주기ms=1] [1/N 유실=8]" >&2; exit 1; }
CYCLE="${2:-1}"
EVERY="${3:-8}"
PORT="${PORT:-30516}"

SERVER_IP=$(python3 -c "import json; print(json.load(open('config/vsomeip-hybrid-recovery-service.json'))['unicast'])")
[ "$CLIENT_IP" != "$SERVER_IP" ] || {
    echo "<클라이언트 IP>에 이 보드 자신의 주소($SERVER_IP)를 줬다 - 상대 보드의 IP를 줘야 한다" >&2
    exit 1
}

pgrep -f build/examples/response-hybrid-recovery >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

cleanup() {
    EVERY="$EVERY" PORT="$PORT" ./tcp-recovery/inject_loss.sh stop "$SERVER_IP" "$CLIENT_IP" >/dev/null 2>&1 || true
}
trap cleanup EXIT

EVERY="$EVERY" PORT="$PORT" ./tcp-recovery/inject_loss.sh start "$SERVER_IP" "$CLIENT_IP"

export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-hybrid-recovery-service.json"
export VSOMEIP_APPLICATION_NAME="service-sample"
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

build/examples/response-hybrid-recovery --cycle "$CYCLE"

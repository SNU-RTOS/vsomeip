#!/bin/bash

# TCP 패킷 복구 시간 측정 - 서버. Thor에서 sudo로 실행. 짝: request_tcp.sh (Orin에서 실행).
#
# 이 스크립트가 유실 주입까지 관리한다 - request_sd.sh/response_sd.sh처럼 양쪽에서 스크립트 하나씩만
# 실행하면 된다 (SSH로 서로를 조종할 필요 없음). 결과는 request_tcp.sh 쪽 로그의
# "패킷 유실 복구 시간: Xus" 줄에서 직접 확인 - 원리는 tcp-recovery/README.md 참고.
#
# 사용법: sudo ./response_tcp.sh <클라이언트 IP> [주기ms=5] [1/N 유실=8]
#   예:   sudo ./response_tcp.sh 192.168.196.122 5 8
# 유실 주입 없이 연결만 확인하려면: NO_DROP=1 sudo ./response_tcp.sh <클라이언트 IP>
set -e

[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행하세요 (유실 주입에 iptables/ip route 필요)" >&2; exit 1; }
CLIENT_IP=$1
[ -n "$CLIENT_IP" ] || { echo "usage: sudo $0 <클라이언트 IP> [주기ms=5] [1/N 유실=8]" >&2; exit 1; }
CYCLE="${2:-5}"
EVERY="${3:-8}"
NO_DROP="${NO_DROP:-0}"

SERVER_IP=$(python3 -c "import json; print(json.load(open('config/vsomeip-tcp-service.json'))['unicast'])")

cleanup() {
    [ "$NO_DROP" = 1 ] || \
        EVERY="$EVERY" ./tcp-recovery/inject_loss.sh stop "$SERVER_IP" "$CLIENT_IP" >/dev/null 2>&1 || true
}
trap cleanup EXIT

if [ "$NO_DROP" != 1 ]; then
    EVERY="$EVERY" ./tcp-recovery/inject_loss.sh start "$SERVER_IP" "$CLIENT_IP"
fi

export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-tcp-service.json"
export VSOMEIP_APPLICATION_NAME="service-sample"

build/examples/response-tcp-recovery --cycle "$CYCLE"

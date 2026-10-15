#!/bin/bash

# RPC timeout+retry 테스트 - 서버. sudo로 실행(유실 주입에 iptables/ip route 필요).
# 짝: request_rpc_retry.sh (반대쪽 보드에서 실행).
#
# 이벤트 스트림이 전혀 없다 - 순수하게 request/response 하나만 반복한다. 유실은 이
# 서비스의 UDP 포트(기본 30518)에 주입된다(서버->클라이언트 응답 쪽).
#
# 사용법: sudo ./response_rpc_retry.sh <클라이언트 IP> [1/N 유실=8]
#   --timeout-us를 고를 때 참고할 "평소 RTT"가 필요하면, 유실 주입 없이 먼저 재볼 것:
#   NO_DROP=1 sudo ./response_rpc_retry.sh <클라이언트 IP>
set -e

[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행하세요 (유실 주입에 iptables/ip route가 필요)" >&2; exit 1; }
CLIENT_IP=$1
[ -n "$CLIENT_IP" ] || { echo "usage: sudo $0 <클라이언트 IP> [1/N 유실=8]" >&2; exit 1; }
EVERY="${2:-8}"
PORT="${PORT:-30518}"
NO_DROP="${NO_DROP:-0}"

SERVER_IP=$(python3 -c "import json; print(json.load(open('config/vsomeip-rpc-retry-service.json'))['unicast'])")
[ "$CLIENT_IP" != "$SERVER_IP" ] || {
    echo "<클라이언트 IP>에 이 보드 자신의 주소($SERVER_IP)를 줬다 - 상대 보드의 IP를 줘야 한다" >&2
    exit 1
}

pgrep -f build/examples/response-rpc-retry >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

cleanup() {
    [ "$NO_DROP" = 1 ] || \
        PROTO=udp PORT="$PORT" EVERY="$EVERY" ./tcp-recovery/inject_loss.sh stop "$SERVER_IP" "$CLIENT_IP" >/dev/null 2>&1 || true
}
trap cleanup EXIT

if [ "$NO_DROP" != 1 ]; then
    PROTO=udp PORT="$PORT" EVERY="$EVERY" ./tcp-recovery/inject_loss.sh start "$SERVER_IP" "$CLIENT_IP"
fi

export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-rpc-retry-service.json"
export VSOMEIP_APPLICATION_NAME="service-sample"
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

build/examples/response-rpc-retry

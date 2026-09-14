#!/bin/bash

# TCP 패킷 복구 시간 측정 - 서버. Thor에서 sudo로 실행. 짝: request_tcp.sh (Orin에서 실행).
#
# 이 스크립트가 유실 주입까지 관리한다 - request_sd.sh/response_sd.sh처럼 양쪽에서 스크립트 하나씩만
# 실행하면 된다 (SSH로 서로를 조종할 필요 없음). 결과는 request_tcp.sh 쪽 로그의
# "패킷 유실 복구 시간: Xus" 줄에서 직접 확인 - 원리는 tcp-recovery/README.md 참고.
#
# 사용법: sudo ./response_tcp.sh <클라이언트 IP> [주기ms=1] [1/N 유실=8]
#   예:   sudo ./response_tcp.sh 10.10.10.2 1 8
#   주기를 왜 1ms(짧게)로 잡는지는 request_tcp.sh 상단 주석 참고 - request_tcp.sh에 준 값과
#   같아야 한다.
# 유실 주입 없이 연결만 확인하려면: NO_DROP=1 sudo ./response_tcp.sh <클라이언트 IP>
set -e

[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행하세요 (유실 주입에 iptables/ip route 필요)" >&2; exit 1; }
CLIENT_IP=$1
[ -n "$CLIENT_IP" ] || { echo "usage: sudo $0 <클라이언트 IP> [주기ms=1] [1/N 유실=8]" >&2; exit 1; }
CYCLE="${2:-1}"
EVERY="${3:-8}"
NO_DROP="${NO_DROP:-0}"

SERVER_IP=$(python3 -c "import json; print(json.load(open('config/vsomeip-tcp-service.json'))['unicast'])")
[ "$CLIENT_IP" != "$SERVER_IP" ] || {
    echo "<클라이언트 IP>에 이 보드 자신의 주소($SERVER_IP)를 줬다 - 상대(Orin 등) 보드의 IP를 줘야 한다" >&2
    exit 1
}

# 이전 실행이 kill -9 등으로 비정상 종료되며 남긴 소켓/잠금파일이 있으면 새로 시작하기 전에 정리한다.
# 안 지우면 "Could not open /tmp/vsomeip.lck: Permission denied" 로 초기화 자체가 조용히 실패하고
# (vsomeip 라이브러리가 그 뒤 비정상 종료까지 이어짐) 원인을 알기 어렵다. 같은 바이너리가 실제로
# 아직 돌고 있으면 건드리지 않는다.
pgrep -x response-tcp-recovery >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

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
# 게이트웨이 없는 직결 링크에서는 netlink의 "기본 라우트" 이벤트가 오지 않아 이게 없으면 SD 자체가
# 시작되지 않는다 (response_sd.sh 참고). 끄려면: sudo VSOMEIP_SD_FAST_START=0 ./response_tcp.sh ...
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

build/examples/response-tcp-recovery --cycle "$CYCLE"

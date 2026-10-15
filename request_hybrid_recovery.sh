#!/bin/bash

# 하이브리드 복구 테스트 - 클라이언트. 짝: response_hybrid_recovery.sh (반대쪽 보드에서
# sudo로 실행).
#
# TCP 이벤트 스트림은 그대로 유지하면서(request-tcp-recovery.cpp와 동일한 자기보정
# baseline + circuit breaker), 수신 간격이 너무 길어졌다고 "사전에"(watchdog 스레드가
# 능동적으로) 판단하는 즉시 - 그 TCP 데이터가 실제로 도착하기 전에 - UDP 복구 RPC를
# 쏴서 그 왕복 시간을 "하이브리드 복구 시간"으로 잰다. 같은 사건에 대해 TCP 자체가
# 실제로 언제 따라잡는지도 "참고용"으로 같이 찍는다 - 두 숫자를 나란히 비교하는 게
# 이 테스트의 목적.
#
# 사용법: ./request_hybrid_recovery.sh [주기ms=1] [임계배수=3.0] [최소유실건수=0]
export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-hybrid-recovery-client.json"
export VSOMEIP_APPLICATION_NAME="client-sample"
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

CYCLE="${1:-1}"
THRESHOLD="${2:-3.0}"
MIN_LOSSES="${3:-0}"

pgrep -f build/examples/request-hybrid-recovery >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

build/examples/request-hybrid-recovery --cycle "$CYCLE" --threshold "$THRESHOLD" --min-losses "$MIN_LOSSES"

#!/bin/bash

# RPC timeout+retry 테스트 - 클라이언트. 짝: response_rpc_retry.sh (반대쪽 보드에서
# sudo로 실행).
#
# 요청 하나 보내고 --timeout-us 안에 응답이 없으면 클라이언트가 직접 재전송한다 -
# 그 재전송 시점부터 응답을 받을 때까지가 "복구 시간"이다.
#
# 사용법: ./request_rpc_retry.sh [주기ms=5] [timeout_us=2000] [최대재전송=5] [최소복구건수=10]
export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-rpc-retry-client.json"
export VSOMEIP_APPLICATION_NAME="client-sample"
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

CYCLE="${1:-5}"
TIMEOUT_US="${2:-2000}"
MAX_RETRIES="${3:-5}"
MIN_RECOVERIES="${4:-10}"

pgrep -f build/examples/request-rpc-retry >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

build/examples/request-rpc-retry --cycle "$CYCLE" --timeout-us "$TIMEOUT_US" --max-retries "$MAX_RETRIES" --min-recoveries "$MIN_RECOVERIES"

#!/bin/bash

# TCP 패킷 복구 시간 측정 - 클라이언트. Orin에서 실행. 짝: response_tcp.sh (Thor에서 sudo로 실행).
#
# 아래 화면 로그의 "패킷 유실 복구 시간: Xus" 줄이 이번 실행에서 잰 값이다 - 서버가 유실시킨
# 세그먼트를 재전송하고, 그게 이 클라이언트에 도착하기까지 걸린 시간. request_sd.sh의
# "매칭까지 처리 시간"과 같은 방식으로, 이 클라이언트 자신이 직접 재는 값이다 (원리와 tcpdump
# 기반 교차검증 방법은 tcp-recovery/README.md 참고).
#
# 사용법: ./request_tcp.sh [주기ms=5] [임계배수=2.0]
#   주기(ms)는 response_tcp.sh에 준 값과 같아야 한다 - 다르면 임계값 판단이 어긋난다.
#   임계배수: 정상 주기의 몇 배 이상 수신 간격이 벌어지면 "복구"로 볼지. 기본 2.0.
#   이 링크에서 유실 없이 평소 얼마나 벌어지는지 먼저 관찰한 뒤(NO_DROP=1로 켠 response_tcp.sh
#   상대로 실행), 그보다 확실히 큰 값으로 조정할 것.

export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-tcp-client.json"
export VSOMEIP_APPLICATION_NAME="client-sample"

CYCLE="${1:-5}"
THRESHOLD="${2:-2.0}"

build/examples/request-tcp-recovery --cycle "$CYCLE" --threshold "$THRESHOLD"

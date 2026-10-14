#!/bin/bash

# UDP "fast recovery" 프로토타입 - 클라이언트. 짝: response_udp_recovery.sh (반대쪽 보드에서
# sudo로 실행).
#
# request-tcp-recovery.cpp와 비교하기 위한 도구다 - "복구 시간"의 정의는 똑같이 "유실을
# 인지한 시점부터 복구 데이터가 손에 들어올 때까지"지만, 측정 방식이 다르다:
#   - TCP 버전: 수신 간격을 보는 휴리스틱(자기보정 baseline) - 오탐 가능성이 있어 두 번이나
#     버그를 재현·수정해야 했다(CHANGES_THOR.md 참고).
#   - 이 버전: UDP 시퀀스 번호의 불연속을 직접 본다 - 유실 여부에 결정론적이라 오탐이
#     원천적으로 없다. 유실을 감지하면 바로 별도 RPC로 "복구"를 요청하고 그 왕복 시간을
#     그대로 잰다 - 커널 TCP 재전송이 아니라 우리가 만든 로직이 복구를 담당.
#
# 두 도구의 수치를 나란히 비교해서, TCP 쪽에서 막힌 ~2ms+ 구간이 TCP 스택 자체의 비용인지
# (그럼 이 UDP 버전이 더 빠를 것) 아니면 이 호스트의 모든 소켓이 겪는 공통 비용인지(그럼
# 비슷하게 나올 것) 확인하는 게 이 프로토타입의 목적.
#
# 사용법: ./request_udp_recovery.sh [주기ms=1] [최소유실건수=10]
#   최소유실건수: 이만큼 복구에 성공하면 스스로 멈추고 "총 수신 N건 중 M건 유실, 평균
#   복구 시간 Xus"를 찍는다. 기본 10. 0을 주면 서버가 세션을 끝낼 때까지(또는 Ctrl-C)
#   계속 돈다.
export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-udp-recovery-client.json"
export VSOMEIP_APPLICATION_NAME="client-sample"
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

CYCLE="${1:-1}"
MIN_LOSSES="${2:-10}"

# -f를 쓰는 이유는 request_tcp.sh와 같다: "request-udp-recovery"가 15자를 넘어
# pgrep -x(15자에서 잘림)로는 못 찾는다.
pgrep -f build/examples/request-udp-recovery >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

build/examples/request-udp-recovery --cycle "$CYCLE" --min-losses "$MIN_LOSSES"

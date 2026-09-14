#!/bin/bash

# TCP 패킷 복구 시간 측정 - 클라이언트. Orin에서 실행. 짝: response_tcp.sh (Thor에서 sudo로 실행).
#
# 아래 화면 로그의 "패킷 유실 복구 시간: Xus" 줄이 이번 실행에서 잰 값이다 - 서버가 유실시킨
# 세그먼트를 재전송하고, 그게 이 클라이언트에 도착하기까지 걸린 시간. request_sd.sh의
# "매칭까지 처리 시간"과 같은 방식으로, 이 클라이언트 자신이 직접 재는 값이다 (원리와 tcpdump
# 기반 교차검증 방법은 tcp-recovery/README.md 참고).
#
# 사용법: ./request_tcp.sh [주기ms=1] [임계배수=3.0]
#   주기(ms)는 response_tcp.sh에 준 값과 같아야 한다 - 다르면 임계값 판단이 어긋난다.
#   임계배수: 실제 수신 간격이 [주기 x 임계배수]를 넘으면 "복구"로 본다. 기본 3.0.
#
#   주기를 1ms처럼 짧게 잡는 이유: vsomeip의 TCP 발행 자체가 요청한 주기와 무관하게 대략
#   일정한 비율(이 환경에서 실측 ~15%)로만 나간다 - 주기를 길게 잡아도 이 비율은 그대로라
#   "처리량을 정상화"하는 효과가 없다. 반대로 짧게 잡을수록 실제 발행 간격이 짧아져서, 유실이
#   발생했을 때 다음 정상 이벤트("트리거")가 더 빨리 도착한다 - 즉 복구 감지가 더 촘촘해지고
#   측정값이 순수 네트워크 지연(tcp-recovery/analyze_recovery.py의 와이어 레벨 값)에 더
#   가까워진다. 실측: --cycle 5 --threshold 2.0 → 평균 7.4~7.8ms, --cycle 1 --threshold 3.0
#   → 평균 2.7ms (와이어 레벨 2.4~2.7ms와 거의 일치). 자세한 내용은 tcp-recovery/README.md
#   "발행 주기가 왜 중요한가" 참고.
#
#   이 링크에서 유실 없이 평소 얼마나 벌어지는지 먼저 관찰한 뒤(NO_DROP=1로 켠 response_tcp.sh
#   상대로 실행), 그보다 확실히 큰 값으로 임계배수를 조정할 것 - 주기를 바꾸면 반드시 다시 확인.

export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-tcp-client.json"
export VSOMEIP_APPLICATION_NAME="client-sample"
# 게이트웨이 없는 직결 링크에서는 netlink의 "기본 라우트" 이벤트가 오지 않아 이게 없으면 SD 자체가
# 시작되지 않는다 (response_sd.sh 참고). 끄려면: VSOMEIP_SD_FAST_START=0 ./request_tcp.sh ...
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

CYCLE="${1:-1}"
THRESHOLD="${2:-3.0}"

build/examples/request-tcp-recovery --cycle "$CYCLE" --threshold "$THRESHOLD"

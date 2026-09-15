#!/bin/bash

# TCP 패킷 복구 시간 측정 - 클라이언트. Orin에서 실행. 짝: response_tcp.sh (Thor에서 sudo로 실행).
#
# 아래 화면 로그의 "패킷 유실 복구 시간: Xus" 줄이 이번 실행에서 잰 값이다 - 서버가 유실시킨
# 세그먼트를 재전송하고, 그게 이 클라이언트에 도착하기까지 걸린 시간. request_sd.sh의
# "매칭까지 처리 시간"과 같은 방식으로, 이 클라이언트 자신이 직접 재는 값이다 (원리와 tcpdump
# 기반 교차검증 방법은 tcp-recovery/README.md 참고).
#
# 이 클라이언트 자신의 "유실 건수"는 수신 간격을 보는 휴리스틱이라 진짜 유실 건수와 정확히
# 일치한다는 보장이 없다 (서버 쪽 tcpdump 기반 와이어 레벨 캡처가 진짜 - raw pcap 대조로
# 검증됨, CHANGES_THOR.md 참고). 그래서 기본 동작이 바뀌었다: 이 클라이언트는 더 이상 자기
# 판단만으로 스스로 멈추지 않고, response_tcp.sh 쪽이 자신의 --min-losses(실제 유실 건수)에
# 도달해 SD 세션을 정상 종료(STOP OFFER)하면 그걸 감지해서 같이 멈춘다 - "실제로 N번 유실"의
# 기준은 항상 서버(response_tcp.sh)의 [wire] 로그다.
#
# 사용법: ./request_tcp.sh [주기ms=1] [임계배수=3.0] [최소유실건수=0]
#   주기(ms)는 response_tcp.sh에 준 값과 같아야 한다 - 다르면 임계값 판단이 어긋난다.
#   임계배수: 실제 수신 간격이 [주기 x 임계배수]를 넘으면 "복구"로 본다. 기본 3.0.
#   최소유실건수: 기본 0 - 서버가 세션을 끝낼 때까지 기다린다(위 설명 참고). 0이 아닌 값을 주면
#   이 클라이언트 자신의 (휴리스틱) 유실 건수가 그만큼 되는 순간에도 추가로 멈춘다 - 이
#   스크립트가 통제하지 않는 서버(세션을 명시적으로 끝내지 않는 일반 SOME/IP 서버 등)를 상대로
#   쓸 때 필요.
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
MIN_LOSSES="${3:-0}"

# 이전 실행이 kill -9 등으로 비정상 종료되며 남긴 소켓/잠금파일이 있으면 정리 - 안 지우면
# "Could not open /tmp/vsomeip.lck: Permission denied"로 초기화가 조용히 실패한다. 이전
# 실행을 sudo로 돌렸다면 그 파일은 root 소유라 이 줄(비-root)로는 못 지운다 - 그럴 땐
# sudo rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 을 직접 실행할 것.
# -f를 쓰는 이유: "request-tcp-recovery"가 15자를 넘어 pgrep -x(정확 일치, 15자에서 잘림)로는
# 절대 못 찾는다 - 그러면 "안 돌고 있다"고 항상 잘못 판단해서 이 가드 자체가 무의미해진다.
pgrep -f build/examples/request-tcp-recovery >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

build/examples/request-tcp-recovery --cycle "$CYCLE" --threshold "$THRESHOLD" --min-losses "$MIN_LOSSES"

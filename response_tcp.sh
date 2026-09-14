#!/bin/bash

# TCP 패킷 복구 시간 측정 - 서버. sudo로 실행. 짝: request_tcp.sh (반대쪽 보드에서 실행).
#
# 이 스크립트가 유실 주입까지 관리한다 - request_sd.sh/response_sd.sh처럼 양쪽에서 스크립트 하나씩만
# 실행하면 된다 (SSH로 서로를 조종할 필요 없음). 결과는 두 군데서 볼 수 있다:
#   - request_tcp.sh 쪽 로그의 "패킷 유실 복구 시간: Xus" 줄 - 클라이언트 자신이 직접 잰 값
#     (vsomeip 콜백까지 포함, OS/NIC 수신 지연이 섞여 들어감)
#   - 이 스크립트 자신의 "[wire] ..." 줄 - 서버 쪽 tcpdump를 실시간으로 분석한 와이어 레벨 값
#     (tcp-recovery/analyze_recovery.py --live). 두 지표의 차이와 원리는
#     tcp-recovery/README.md, tcp-recovery/analyze_recovery.py의 모듈 docstring 참고.
# 클라이언트와 마찬가지로 유실을 --min-losses건 감지하면 스스로 요약을 찍고 멈춘다.
#
# 사용법: sudo ./response_tcp.sh <클라이언트 IP> [주기ms=1] [1/N 유실=8] [최소유실건수=10]
#   예:   sudo ./response_tcp.sh 10.10.10.2 1 8 10
#   주기를 왜 1ms(짧게)로 잡는지는 request_tcp.sh 상단 주석 참고 - request_tcp.sh에 준 값과
#   같아야 한다.
# 유실 주입 없이 연결만 확인하려면: NO_DROP=1 sudo ./response_tcp.sh <클라이언트 IP>
#   (이 경우 유실이 없으므로 [wire] 분석은 돌리지 않고, 예전처럼 서버를 포그라운드에서 그냥 실행한다)
set -e

[ "$(id -u)" -eq 0 ] || { echo "sudo로 실행하세요 (유실 주입/tcpdump에 iptables/ip route/pcap 필요)" >&2; exit 1; }
CLIENT_IP=$1
[ -n "$CLIENT_IP" ] || { echo "usage: sudo $0 <클라이언트 IP> [주기ms=1] [1/N 유실=8] [최소유실건수=10]" >&2; exit 1; }
CYCLE="${2:-1}"
EVERY="${3:-8}"
MIN_LOSSES="${4:-10}"
NO_DROP="${NO_DROP:-0}"
PORT="${PORT:-30510}"

SERVER_IP=$(python3 -c "import json; print(json.load(open('config/vsomeip-tcp-service.json'))['unicast'])")
[ "$CLIENT_IP" != "$SERVER_IP" ] || {
    echo "<클라이언트 IP>에 이 보드 자신의 주소($SERVER_IP)를 줬다 - 상대(Orin 등) 보드의 IP를 줘야 한다" >&2
    exit 1
}

# 이전 실행이 kill -9 등으로 비정상 종료되며 남긴 소켓/잠금파일이 있으면 새로 시작하기 전에 정리한다.
# 안 지우면 "Could not open /tmp/vsomeip.lck: Permission denied" 로 초기화 자체가 조용히 실패하고
# (vsomeip 라이브러리가 그 뒤 비정상 종료까지 이어짐) 원인을 알기 어렵다. 같은 바이너리가 실제로
# 아직 돌고 있으면 건드리지 않는다.
# -f를 쓰는 이유: "response-tcp-recovery"가 15자를 넘어 pgrep -x(정확 일치, 15자에서 잘림)로는
# 절대 못 찾는다 - 그러면 "안 돌고 있다"고 항상 잘못 판단해서 이 가드 자체가 무의미해진다.
pgrep -f build/examples/response-tcp-recovery >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

SVC=""
cleanup() {
    for p in $(pgrep -f "tcpdump.*tcp port $PORT"); do kill "$p" 2>/dev/null || true; done
    # SVC(서버 바이너리)를 먼저 곱게 죽이고, 안 죽으면 강제 종료 - kill -9를 이미 죽은 PID에
    # 쓰면 실패하는데 이 함수는 trap EXIT라 set -e 예외 대상이 아니다, 그러니 || true 필수.
    if [ -n "$SVC" ]; then
        kill "$SVC" 2>/dev/null || true
        sleep 0.3
        kill -9 "$SVC" 2>/dev/null || true
    fi
    [ "$NO_DROP" = 1 ] || \
        EVERY="$EVERY" PORT="$PORT" ./tcp-recovery/inject_loss.sh stop "$SERVER_IP" "$CLIENT_IP" >/dev/null 2>&1 || true
}
trap cleanup EXIT

if [ "$NO_DROP" != 1 ]; then
    EVERY="$EVERY" PORT="$PORT" ./tcp-recovery/inject_loss.sh start "$SERVER_IP" "$CLIENT_IP"
fi

export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-tcp-service.json"
export VSOMEIP_APPLICATION_NAME="service-sample"
# 게이트웨이 없는 직결 링크에서는 netlink의 "기본 라우트" 이벤트가 오지 않아 이게 없으면 SD 자체가
# 시작되지 않는다 (response_sd.sh 참고). 끄려면: sudo VSOMEIP_SD_FAST_START=0 ./response_tcp.sh ...
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

if [ "$NO_DROP" = 1 ]; then
    # 유실이 없으니 [wire] 분석은 의미가 없다 - 예전처럼 서버만 포그라운드로 돌린다.
    build/examples/response-tcp-recovery --cycle "$CYCLE"
    exit 0
fi

# 서버를 백그라운드로 돌리고, 이 셸은 그 트래픽을 실시간으로 캡처해 tcp-recovery/analyze_recovery.py
# --live로 분석한 [wire] 요약을 포그라운드에 찍는다 - 클라이언트 쪽 로그의 "패킷 유실 복구 시간"과
# 같은 사건을, 두 대의 시계를 맞출 필요 없이 서버 쪽에서만 교차검증하는 값이다 (원리는
# tcp-recovery/analyze_recovery.py 모듈 docstring 및 README "결과: 실측값" 참고).
build/examples/response-tcp-recovery --cycle "$CYCLE" &
SVC=$!

# inject_loss.sh와 동일한 방식으로 클라이언트로 가는 실제 egress 인터페이스를 구한다 - 기본 라우트가
# 아니라 클라이언트로의 실제 경로를 써야, 유선/Wi-Fi가 같이 있는 멀티홈 상황에서도 엉뚱한
# 인터페이스를 캡처하지 않는다.
IFACE=$(ip route get "$CLIENT_IP" 2>/dev/null | awk '{for(i=1;i<=NF;i++) if($i=="dev"){print $(i+1); exit}}')
IFACE=${IFACE:-$(ip route show default | awk '/default/{print $5; exit}')}

# tcpdump는 익명 파이프가 아니라 FIFO로 analyze_recovery.py에 이어준다 - 익명 파이프(`tcpdump | python3`)로
# 직접 연결하면 analyze_recovery.py가 --min-losses에 먼저 도달해 조용히 끝났을 때, tcpdump는 "다음 패킷이
# 와서 쓰기를 시도할 때"라야 SIGPIPE로 죽는다는 게 문제다 - 클라이언트가 이미 --min-losses에 먼저 도달해
# 스스로 멈추고 연결을 끊은 경우(흔한 케이스: 두 min-losses가 정확히 같은 사건 수에 도달한다는 보장이
# 없다) 그 뒤로는 해당 포트에 트래픽 자체가 없어 tcpdump가 영원히 쓰기를 시도하지 않고, 그러면
# SIGPIPE도 영원히 오지 않아 tcpdump가 좀비처럼 남고 - 이 스크립트도 파이프라인이 안 끝나 트랩까지
# 못 가 서버 프로세스와 유실 주입 규칙까지 같이 남는다. FIFO는 analyze_recovery.py가 끝나는 즉시(입력이
# 남았든 안 남았든) 아래 줄로 제어가 돌아오므로, tcpdump가 스스로 죽기를 기다리지 않고 바로 명시적으로
# 죽인다.
FIFO=$(mktemp -u /tmp/tcp-recovery-live.XXXXXX)
mkfifo "$FIFO"
tcpdump -i "$IFACE" -l -n -S -tt --time-stamp-precision=micro "tcp port $PORT" > "$FIFO" 2>/dev/null &
TCPDUMP_PID=$!

python3 tcp-recovery/analyze_recovery.py --live --port "$PORT" --min-losses "$MIN_LOSSES" < "$FIFO"

kill "$TCPDUMP_PID" 2>/dev/null || true
rm -f "$FIFO"

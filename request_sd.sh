#!/bin/bash

# 환경 변수 설정
export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-udp-client.json"
export VSOMEIP_APPLICATION_NAME="client-sample"
# ST_REGISTERED 직후 netlink 응답을 기다리지 않고 SD 시작 (routing_manager_impl::start 참고)
# 끄고 비교하려면: VSOMEIP_SD_FAST_START=0 ./request_sd.sh
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

BINARY_PATH="build/examples/request-sd"

# 이전 실행이 kill -9 등으로 비정상 종료되며 남긴 소켓/잠금파일이 있으면 정리 - 안 지우면
# "Could not open /tmp/vsomeip.lck: Permission denied"로 첫 반복부터 초기화가 실패한다.
pgrep -x request-sd >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

total_time=0
count=0

for i in {1..100}
do
    # 바이너리 실행, 출력은 임시 파일에 저장
    $BINARY_PATH > temp_output.txt 2>&1 &
    PID=$!

    sleep 2

    if kill -0 $PID 2>/dev/null; then
        kill $PID
        wait $PID 2>/dev/null
    fi

    # -m1: on_availability()가 간혹(재전송된 중복 OfferService 등으로) 두 번 이상 로그를
    # 남기면 TIME이 여러 줄이 되어 아래 bc 계산이 깨진다 - 그 중 첫 줄(최초 도달 시각 기준)만 쓴다.
    TIME=$(grep -m1 '매칭까지 처리 시간' temp_output.txt | awk -F' ' '{print $NF}' | tr -d 'us')
    echo "실행 횟수: $i, 처리 시간: $TIME us"
    if [[ $TIME =~ ^[0-9]+$ ]]; then
        total_time=$(echo "$total_time + $TIME" | bc)
        ((count++))
    fi
done

if [[ $count -gt 0 ]]; then
    average=$(echo "scale=2; $total_time / $count" | bc)
    echo "평균 처리 시간: ${average}us"
else
    echo "처리 시간을 계산할 수 없습니다."
fi

rm -f temp_output.txt
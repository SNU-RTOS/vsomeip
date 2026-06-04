#!/bin/bash

# 환경 변수 설정
export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-udp-client.json"
export VSOMEIP_APPLICATION_NAME="client-sample"

BINARY_PATH="build/examples/request-sd"

total_time=0
count=0

for i in {1..10}
do
    # 바이너리 실행, 출력은 임시 파일에 저장
    $BINARY_PATH > temp_output.txt 2>&1 &
    PID=$!

    sleep 2

    if kill -0 $PID 2>/dev/null; then
        kill $PID
        wait $PID 2>/dev/null
    fi

    TIME=$(grep '매칭까지 처리 시간' temp_output.txt | awk -F' ' '{print $NF}' | tr -d 'us')
    echo "실행 횟수: $i, 처리 시간: $TIME us"
    if [[ -n $TIME ]]; then
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
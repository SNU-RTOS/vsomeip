#!/bin/bash

# TCP 패킷 복구 시간 측정용 클라이언트 (SOME/IP 이벤트 구독자).
# 짝: response_tcp.sh (notify-sample). 이유는 그쪽 스크립트의 주석과
# tcp-recovery/README.md "트래픽 패턴이 중요하다" 참고.
#
# 유실 주입과 측정은 tcp-recovery/ 참고 - ./tcp-recovery/run_experiment.sh가 이 둘을 유실
# 주입·캡처와 함께 자동으로 실행해준다. 이 스크립트는 유실 주입 없이 연결만 확인하고 싶을 때,
# 또는 run_experiment.sh 없이 수동으로 조합할 때 쓴다.

export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-tcp-client.json"
export VSOMEIP_APPLICATION_NAME="client-sample"

build/examples/subscribe-sample --tcp

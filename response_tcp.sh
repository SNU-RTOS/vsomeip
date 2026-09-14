#!/bin/bash

# TCP 패킷 복구 시간 측정용 서버(SOME/IP 이벤트 발행자).
# 유실 뒤로도 트래픽이 이어지는 패턴이어야 클라이언트가 SACK으로 유실을 알아챌 수 있다 - 이유는
# tcp-recovery/README.md "트래픽 패턴이 중요하다" 참고. 한 번의 요청-응답이 끝나면 트래픽이 끊기는
# request-sample/response-sample(락스텝)은 이 목적에 맞지 않는다.
#
# 짝: request_tcp.sh (subscribe-sample). 유실 주입과 측정은 tcp-recovery/ 참고 -
# ./tcp-recovery/run_experiment.sh가 이 둘을 유실 주입·캡처와 함께 자동으로 실행해준다.

export LD_LIBRARY_PATH="build:$LD_LIBRARY_PATH"
export VSOMEIP_CONFIGURATION="config/vsomeip-tcp-service.json"
export VSOMEIP_APPLICATION_NAME="service-sample"

CYCLE_MS="${1:-5}"

build/examples/notify-sample --cycle "$CYCLE_MS"

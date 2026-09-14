# ST_REGISTERED 직후 netlink의 "기본 라우트" 이벤트를 기다리지 않고 SD 시작
# (routing_manager_impl::start 참고). 게이트웨이가 없는 직결 링크(예: Thor<->Orin 이더넷
# 다이렉트 연결)에서는 이 이벤트가 영원히 오지 않아 이게 없으면 SD 자체가 시작되지 않는다.
# 끄고 비교하려면: VSOMEIP_SD_FAST_START=0 ./response_sd.sh
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"

# 이전 실행이 kill -9 등으로 비정상 종료되며 남긴 소켓/잠금파일이 있으면 정리 - 안 지우면
# "Could not open /tmp/vsomeip.lck: Permission denied"로 초기화가 조용히 실패한다. 같은
# 바이너리가 실제로 아직 돌고 있으면 건드리지 않는다.
pgrep -x response-sd >/dev/null 2>&1 || rm -f /tmp/vsomeip-0 /tmp/vsomeip.lck 2>/dev/null

env LD_LIBRARY_PATH=build:$LD_LIBRARY_PATH VSOMEIP_CONFIGURATION=config/vsomeip-udp-service.json VSOMEIP_APPLICATION_NAME=service-sample build/examples/response-sd

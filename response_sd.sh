# ST_REGISTERED 직후 netlink의 "기본 라우트" 이벤트를 기다리지 않고 SD 시작
# (routing_manager_impl::start 참고). 게이트웨이가 없는 직결 링크(예: Thor<->Orin 이더넷
# 다이렉트 연결)에서는 이 이벤트가 영원히 오지 않아 이게 없으면 SD 자체가 시작되지 않는다.
# 끄고 비교하려면: VSOMEIP_SD_FAST_START=0 ./response_sd.sh
export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"
env LD_LIBRARY_PATH=build:$LD_LIBRARY_PATH VSOMEIP_CONFIGURATION=config/vsomeip-udp-service.json VSOMEIP_APPLICATION_NAME=service-sample build/examples/response-sd

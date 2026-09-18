# Service Discovery (SD) 지연 시간 — Baseline vs 최적화 비교

측정 대상: `examples/request-sd.cpp`에서 `ST_REGISTERED`(클라이언트 상태 콜백) →
`on_availability`(서비스 `0x1234.0x5678` 발견 콜백)까지 걸린 시간. Thor/Orin 두 보드
간 SOME/IP-SD(UDP 멀티캐스트 224.244.224.245:30490) 기준.

## 요약

| | Baseline (최적화 전) | 최적화 버전 (2026-09, 유선 직결) |
|---|---|---|
| 평균 | 20ms | **1193us (1.19ms)** |
| p50(중앙값) | - | 1188us |
| p90 | - | 1274us |
| 목표(2ms) 통과율 | - | **99/100 (99%)** |

---

## 1. Latency 분석 — 무엇을, 어떻게 쟀는가

### 1.1 측정 구간의 정의

`ST_REGISTERED`는 vsomeip 런타임이 라우팅을 다 준비하고 애플리케이션에게 "이제
`request_service()`를 불러도 된다"고 알려주는 시점이다. `on_availability`는 그 서비스가
실제로 네트워크상에서 발견되어 사용 가능해진 시점이다. 이 구간은 실질적으로
"SOME/IP-SD FindService를 내보내고 → 상대의 OfferService를 받아 매칭 확인까지"의 전체
과정을 포함한다 — 사용자 애플리케이션이 실제로 체감하는 "서비스를 쓸 수 있게 되기까지의
시간"과 일치하도록 이 구간을 골랐다.

### 1.2 구간 분해 (tcpdump와 클라이언트 로그 타임스탬프 상관분석)

| 구간 | 내용 | 비중(유선 직결 기준) |
|---|---|---|
| `ST_REGISTERED` → FindService가 실제로 wire에 나가는 시점 | netlink 이벤트 대기(개선 전) 또는 즉시 송신(개선 후) | 최적화 전 330~776us, 최적화 후 ~50us |
| **FindService 송신 → OfferService 수신** | **물리 왕복 지연 + 상대 보드의 처리 시간** | 대부분(예: Wi-Fi 시절 1750~2230us) |
| OfferService 수신 → `on_availability` 콜백 | vsomeip 내부 디스패처 스레드로 넘어가는 오버헤드 | 130~250us |

즉 전체 지연의 절대다수는 "FindService → OfferService" 구간, 즉 **물리 링크의 왕복
지연**이 차지한다. 나머지 두 구간(SD 시작 지연, 디스패처 홉)은 절대량 자체는 작지만
쌓이면 마이크로초 단위로 계속 남는, "0으로 만들 수 있는" 오버헤드였다.

---

## 2. 지연에 영향을 줄 수 있는 요인 분석

| 요인 | 영향 방식 | 근거 |
|---|---|---|
| **물리 링크 종류(Wi-Fi vs 유선)** | 가장 지배적 — Wi-Fi RTT 1.5~1.9ms vs 같은 스위치의 유선 호스트 0.24ms | ICMP 왕복 실측, `vsomeip-sd-latency-breakdown` 메모 |
| **SD 시작 타이밍(netlink 대기)** | Linux에서 `routing_manager_impl::start()`가 인터페이스/라우트 netlink 이벤트를 기다린 뒤에야 SD를 시작 — 이 대기가 330~776us | 코드 레벨 확인 (`routing_manager_impl.cpp`) |
| **SD 프로토콜 자체의 랜덤 지연 파라미터** | `initial_delay`(최초 오퍼/파인드 전 무작위 대기), find/offer debounce 타이머 — 스펙상 트래픽 폭주 방지용이지만 2대 환경에선 불필요한 지연 | vsomeip SD 스펙(SIP_SD_89/90), 설정 파일 |
| **SD 응답 방식(unicast vs multicast)** | 스펙대로면 상황에 따라 멀티캐스트로 응답 가능 — OS 멀티캐스트 join/처리 오버헤드가 유니캐스트보다 큼 | `send_uni_or_multicast_offerservice` 코드 |
| **CPU 전력/클럭 상태** | idle state(`cc7`, 진입/탈출 지연 최대 10,000us), 주파수 스케일링(`schedutil`)이 응답 처리 자체를 지연시킬 수 있음 | `tune-latency.sh` 전/후 ICMP 실측 |
| **Wi-Fi 전력 절약 모드** | AP가 절전 중인 스테이션에게 프레임을 버퍼링했다가 나중에 보냄 — 지연의 큰 부분 | Orin ICMP 평균 5.47→1.73ms |
| **게이트웨이 없는 점대점 링크의 특수성** | 기본 라우트 netlink 이벤트가 아예 오지 않는 구성 — SD가 "느린 게 아니라 아예 시작이 안 되는" 완전히 다른 실패 모드를 유발 | 유선 직결 전환 시 재발견 |
| **네트워크 잡음/재전송** | 드물게 FindService 패킷이 유실되면 `repetitions_base_delay`(기본 200ms) 재시도 대기 발생 — 극단적 아웃라이어의 원인 | 100회 중 1회 500ms 아웃라이어 관측 |
| **측정 스크립트 자체의 정확도** | 평균 계산 스크립트에 버그가 있으면 실제로는 정상인 측정값도 왜곡되어 보임 | `request_sd.sh` grep 버그 발견·수정 |

---

## 3. 무엇을 고쳤고, 그 결과는 무엇인가

| # | 수정 | 파일/위치 | 효과(실측) |
|---|---|---|---|
| 1 | `initial_delay_min/max`를 서버·클라이언트 양쪽 다 `0`으로 | `config/vsomeip-udp-{service,client}.json` | 시작 전 대기(1~5ms) 제거 |
| 2 | SD 응답을 항상 unicast로 강제 | `implementation/service_discovery/src/service_discovery_impl.cpp` | 멀티캐스트 오버헤드 제거 (2대 점대점 환경 한정 최적) |
| 3 | `VSOMEIP_SD_FAST_START` 추가 — netlink 이벤트를 기다리지 않고 SD 시작 | `implementation/routing/src/routing_manager_impl.cpp` (opt-in env var) | 같은 호스트: 1349→1116us / Wi-Fi: 2724→2488us(중앙값) |
| 4 | Wi-Fi 절전모드 끄기, CPU governor=performance, idle state 비활성화 | `tune-latency.sh` | Orin ICMP 평균 5.47→1.73ms, 1.78→1.52ms(추가 CPU 튜닝) |
| 5 | **Wi-Fi → 이더넷 직결로 물리 링크 교체** | 하드웨어 배선 + NetworkManager 고정 IP 프로파일 | **가장 큰 한 방**: 평균 ~3100us(Wi-Fi) → 1193us(유선), 목표(2ms) 통과율 0%→99% |
| 6 | 점대점 링크에서 `VSOMEIP_SD_FAST_START`가 서버(`response_sd.sh`) 쪽엔 빠져 있던 것 발견·수정 | `response_sd.sh` 외 관련 스크립트 | 이 링크에서는 "느려지는" 정도가 아니라 **SD 자체가 영원히 시작 안 되는** 완전한 실패였음 — 필수 수정 |
| 7 | 측정 스크립트의 평균 계산 버그 수정 | `request_sd.sh` (`grep -m1` + 숫자 검증) | 측정값 자체의 신뢰도 확보(수정 전엔 평균이 통째로 깨질 수 있었음) |

**최종 결과**: 100회 중 99회가 2ms 목표를 통과(1회는 재현 안 되는 500ms대 드문 아웃라이어,
재실행 시 사라짐 — `repetitions_base_delay` 재시도 케이스로 추정). 평균 1.19ms, p90도
1.27ms로 매우 안정적.

---

## 4. 이 변경들은 Service Discovery 단계에만 영향을 준다

SD 관련 수정 항목(1~3, 6번)은 전부 **SOME/IP-SD 프로토콜 계층에서만 동작**하며, 서비스가
일단 발견된 **이후의 실제 통신(RPC 요청/응답, 이벤트 구독·발행, TCP 데이터 전송 등)에는
관여하지 않는다**:

- `initial_delay`, offer/find debounce 타이머는 SD 메시지(FindService/OfferService/
  Subscribe 등)를 **언제 내보내는지**만 결정하는 파라미터다. 서비스가 발견된 뒤 실제로
  주고받는 이벤트/RPC 페이로드의 전송 경로나 타이밍에는 아무 영향이 없다.
- `send_uni_or_multicast_offerservice` 수정은 **SD 프로토콜의 OfferService 응답
  방식**만 바꾼다. 실제 이벤트 데이터(notify)가 유니캐스트로 가는지 멀티캐스트로 가는지는
  `eventgroup`의 별도 설정으로 결정되며, 이번 수정과는 무관하고 손대지 않았다.
- `VSOMEIP_SD_FAST_START`는 SD를 **언제 시작하는지**(netlink 이벤트 대기 스킵)만 바꾼다.
  SD 상태 머신 자체나 이후 RPC/이벤트 경로의 동작 방식은 표준 vsomeip 그대로다.
- `cyclic_offer_delay`를 아주 길게(서버 1,000,000ms, 클라이언트 20,000,000ms) 잡은
  것은 벤치마크 중 "재광고"가 측정을 방해하지 않게 하려는 **테스트 설정**일 뿐이며, 이미
  발견된 서비스와의 통신에는 아무 영향이 없다(재광고는 새로 등장하는 클라이언트를 위한
  것이다).

**한 가지 솔직히 짚어야 할 한계**: unicast-always 응답(2번)은 스펙상 "다른 리스너에게도
동시에 알리기 위한" 멀티캐스트 응답 경로를 이 프로젝트의 **2대 점대점 토폴로지에 한해**
포기한 것이다. 3대 이상이 같은 네트워크에 있는 배포라면, 한 클라이언트의 FindService에
대한 응답이 다른 클라이언트들에게는 즉시 전파되지 않고 다음 주기적 멀티캐스트 오퍼
(`cyclic_offer_delay`)까지 기다려야 할 수 있다 — 지금 설정(수백만 ms)으로는 사실상
"기다리지 않는다"가 아니라 "오지 않는다"에 가까우므로, 다중 클라이언트 배포에 이 설정을
그대로 가져갈 경우 `cyclic_offer_delay`를 현실적인 값으로 되돌려야 한다.

**공통 인프라 변경(유선 직결, `tune-latency.sh`의 CPU/전력 설정)은 SD에 국한되지
않는다** — TCP를 포함한 모든 트래픽에 동일하게 적용되지만, 방향은 항상 개선(지연 감소)
쪽이라 "악영향"이 될 여지가 없다. 오히려 TCP 패킷 복구 시간 쪽도 같은 링크 교체로 크게
개선됐다(`TCP_RECOVERY_REPORT.md` 참고).

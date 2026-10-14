# Thor↔Thor 직결 — SD / TCP 복구 재검증 결과 및 재현 가이드

기존 Thor↔Orin 구성에서 Orin을 또 다른 AGX Thor로 교체하고, 둘을 이더넷으로 직결한 뒤
SD/TCP 복구 테스트를 다시 돌린 결과. "이 보드(Thor)" = 이 저장소를 평소에 작업하던 로컬
머신, "새 Thor" = 이번에 client 역할로 추가된 두 번째 AGX Thor.

## 요약

| | SD (`ST_REGISTERED`→`on_availability`) | TCP 복구, 와이어 레벨(`[C]`) | TCP 복구, 애플리케이션 레벨 |
|---|---|---|---|
| 이 보드=server, 새 Thor=client | mean 1628us / p50 1267us / 99% ≤2ms | mean 312us / p50 351us / 100% ≤2ms | mean 2653us / p50 2662us |
| 새 Thor=server, 이 보드=client | mean 1109us / p50 1094us / 99% ≤2ms | mean 192us / p50 122us / 100% ≤2ms | mean 2661us / p50 2607us |
| (참고) 이전 Thor↔Orin 직결 | mean 1193us / p50 1188us / 99% ≤2ms | mean 552~560us | mean 2753~4865us (방향에 따라 큰 편차) |

**SD, 와이어 레벨 TCP 둘 다 방향과 무관하게 목표(2ms)를 안정적으로 통과.** TCP
애플리케이션 레벨(vsomeip 수신측 처리 포함)은 여전히 ~2.6~2.7ms로 목표를 못 넘지만,
Thor↔Orin 때와 달리 **양방향 수치가 거의 동일**(2653 vs 2661) — 이전엔 어느 쪽이
client냐에 따라 2.7ms~4.9ms까지 차이가 났었다. 두 보드가 같은 NIC(Realtek `r8126`)를
쓰는 지금은 그 비대칭이 사라졌다 — `TCP_RECOVERY_REPORT.md`에서 "Thor의 NIC 드라이버가
원인 후보"라고 짚었던 것과 일치하는 결과.

---

## 1. 환경 구성 (재현 절차)

### 1.1 물리/네트워크

두 Thor를 이더넷 케이블로 직결(스위치·게이트웨이 없음). 각 보드에서 `nmcli`로 고정
IP 프로파일을 만든다 — 일반 `ip addr add`는 NetworkManager가 carrier 상태를 계속
감시하다가 DHCP로 되돌리려 시도해서 유실될 수 있으므로 피한다:

```bash
# 보드 A (이 저장소의 "서버" 역할, 10.10.10.1)
sudo nmcli connection add type ethernet ifname <직결 포트> con-name thor-thor-direct \
    ipv4.method manual ipv4.addresses 10.10.10.1/24 ipv6.method ignore

# 보드 B (새 Thor, "클라이언트" 역할, 10.10.10.2)
sudo nmcli connection add type ethernet ifname <직결 포트> con-name thor-thor-direct \
    ipv4.method manual ipv4.addresses 10.10.10.2/24 ipv6.method ignore
sudo nmcli connection up thor-thor-direct
```

AGX Thor 보드의 직결 포트 이름은 보통 `enP2p1s0`(이 프로젝트의 두 보드 모두 동일).
`ip -4 addr show <iface>`로 `LOWER_UP`(케이블 연결됨)과 할당된 IP를 확인한다. `ping
10.10.10.2`(또는 반대쪽)로 RTT 0.2~0.4ms대가 나오면 정상.

### 1.2 저장소 / 빌드

새로 추가하는 보드마다:

```bash
cd ~/workspace/vsomeip
git switch thor          # 또는 git checkout thor
git pull --ff-only origin thor
cd build && cmake --build . --target examples -j$(nproc)
```

**각 보드가 자기 자신의 IP를 쓰도록 config를 로컬에서(커밋하지 않고) 덮어쓴다** — 이
저장소는 `10.10.10.1`을 추적된 기본값으로 쓰므로, **클라이언트로 쓸 보드에서만** 아래를
실행:

```bash
cd ~/workspace/vsomeip
for f in config/vsomeip-udp-service.json config/vsomeip-udp-client.json \
         config/vsomeip-tcp-service.json config/vsomeip-tcp-client.json; do
  python3 -c "
import json
c = json.load(open('$f'))
c['unicast'] = '10.10.10.2'
json.dump(c, open('$f', 'w'), indent=4)
"
done
```

(반대 방향 테스트를 하려면 두 보드 모두 역할이 바뀌는 것이므로, "서버" 역할을 맡는
쪽이 `10.10.10.1`을 유지하고 "클라이언트" 역할을 맡는 쪽만 `10.10.10.2`로 덮어쓰면
된다 — 매번 바꿔줄 필요 없이, 그냥 **스크립트를 실행하는 방향**만 바꾸면 됨. `response_*.sh`
쪽 보드가 서버다.)

### 1.3 이번에 새로 설치해야 했던 패키지

새 Thor 이미지에는 `tcpdump`가 기본 설치되어 있지 않았다 — `response_tcp.sh`가 내부적으로
쓰므로 **서버 역할을 맡는 쪽에는 반드시 설치**:

```bash
sudo apt-get install -y tcpdump
```

(빠졌을 때의 증상: `response_tcp.sh`가 시작 즉시 "`[wire] 총 발행 0건 중 0건 유실`"을
찍고 멈춰버린다 — tcpdump 자체가 `No such file or directory`로 못 뜨면서 분석 스크립트가
입력 없이 EOF를 만나는 것. 원인 모르고 보면 "유실 주입이 안 된다"처럼 보이니 주의.)

---

## 2. SD 테스트

### 2.1 실행

```bash
# 서버 역할 보드
./response_sd.sh

# 클라이언트 역할 보드 (반대쪽, SSH로 접속해서)
./request_sd.sh      # 100회 반복, 약 200초 소요
```

### 2.2 결과

| 방향 | n | mean | p50 | p90 | max | ≤2ms |
|---|---|---|---|---|---|---|
| 이 보드=서버, 새 Thor=클라이언트 | 100 | 1628us | 1267us | 1654us | 35910us* | 99% |
| 새 Thor=서버, 이 보드=클라이언트 | 100 | 1109us | 1094us | 1257us | 2380us | 99% |

\* 100회 중 1회의 드문 아웃라이어(이전 Thor↔Orin 테스트에서도 100회 중 1회꼴로 재현된
적 있는, FindService 유실 후 `repetitions_base_delay` 재시도로 추정되는 패턴 —
`CHANGES_THOR.md` "Direct Ethernet Link > Results" 참고). p50/p90은 두 방향 모두
안정적으로 2ms를 통과한다.

### 2.3 알아둘 점

`response-sd`를 `kill`(SIGTERM)로 멈추면 가끔 `apport`가 SIGABRT로 받아 코어덤프를
남긴다 — 측정 자체(이미 끝난 100회)에는 영향 없는, 종료 경로의 사소한 기존 버그다(이번에
새로 만든 건 아니고, `response-sd.cpp`는 이번 세션에서 손대지 않았다). `kill -9`로
정리하면 깔끔하게 끝난다.

---

## 3. TCP 패킷 복구 테스트

### 3.1 실행

```bash
# 서버 역할 보드 (sudo 필요 - 유실 주입/tcpdump)
sudo ./response_tcp.sh <반대쪽 보드 IP> 1 8 30
#                        예: 10.10.10.2         ^min-losses(실제 유실 30건에서 멈춤)

# 클라이언트 역할 보드
./request_tcp.sh
```

서버가 와이어에서 실제로 확인한 유실이 30건에 도달하면 스스로 멈추고(`[wire] 총 발행
... 건 유실` 요약), SD 세션을 정상 종료한다 — 클라이언트는 이걸 감지해서 자동으로
따라 멈춘다("서버가 실제 유실 목표치에 도달해 연결을 종료함"). 둘 다 켜두면 알아서
끝난다.

### 3.2 결과

**와이어 레벨** (`[wire] ★ 클라이언트 기준 복구 시간`, 서버 쪽 tcpdump 기반, 두 보드
시계 동기화 불필요):

| 방향 | n | mean | p50 | p90 | max | ≤2ms |
|---|---|---|---|---|---|---|
| 이 보드=서버, 새 Thor=클라이언트 | 30 | 312us | 351us | 370us | 383us | 100% |
| 새 Thor=서버, 이 보드=클라이언트 | 30 | 192us | 122us | 361us | 386us | 100% |

**애플리케이션 레벨** (`request_tcp.sh` 자체 로그, vsomeip 수신측 처리 포함):

| 방향 | n(flagged) | mean | p50 | p90 | max | min | baseline(자동보정) |
|---|---|---|---|---|---|---|---|
| 이 보드=서버, 새 Thor=클라이언트 | 34 | 2653us | 2662us | 2776us | 3570us | 2209us | 1056~1083us (안정) |
| 새 Thor=서버, 이 보드=클라이언트 | 48 | 2661us | 2607us | 2956us | 3293us | 2254us | 1032~1070us (안정) |

### 3.3 Thor↔Orin 때와 비교해서 뭐가 달라졌나

- **와이어 레벨이 더 빨라지고 더 일관적이다.** Thor↔Orin은 양방향 552~560us였는데,
  Thor↔Thor는 192~312us — 거의 반 이하. 두 보드가 **같은 NIC 칩**(Realtek `r8126`,
  이산 PCIe 카드)을 쓰는 점이 유력한 이유다. Orin 쪽 `nvethernet`(Tegra 내장 MAC)과
  섞여 있을 때보다, 같은 칩끼리 마주 보니 더 빠르고 더 대칭적으로 나왔다.
- **애플리케이션 레벨의 방향 간 비대칭이 사라졌다.** Thor↔Orin에서는 Thor가
  client일 때(2753→4865us, p90 11ms대)가 Orin이 client일 때보다 훨씬 느리고
  변동폭도 컸다(`TCP_RECOVERY_REPORT.md`의 "NIC 인터럽트 coalescing/드라이버 차이"
  요인 참고). 지금은 양방향 다 2653~2661us로 거의 동일 — 이전의 비대칭이 "Thor의
  NIC/드라이버가 유독 나쁘다"보다는 "서로 다른 두 NIC 칩이 마주 봤을 때 생기는
  상호작용"에 더 가까웠다는 걸 시사한다. 다만 이건 한 쌍의 보드로 한 번 확인한
  것이라, 일반화하려면 더 많은 보드 조합으로 재확인이 필요하다.
- **애플리케이션 레벨 자체는 여전히 2ms를 못 넘는다** — `CONFIG_HZ=250` 커널 틱
  해상도에 묶인 것으로 추정되는, 커널 재빌드 없이는 해소 안 되는 구간이라는 결론은
  이 새 조합에서도 그대로 유지된다(`TCP_RECOVERY_REPORT.md` 2절 참고).

---

## 4. 전체 재현 체크리스트

```bash
# 1) 양쪽 다: nmcli로 10.10.10.1 / 10.10.10.2 직결 IP 설정, ping으로 확인
# 2) 양쪽 다: git switch thor && git pull --ff-only origin thor
# 3) 양쪽 다: cd build && cmake --build . --target examples -j$(nproc)
# 4) 클라이언트로 쓸 보드에서만: config 4개 파일의 unicast를 10.10.10.2로 로컬 수정
# 5) 서버 역할 보드에만: sudo apt-get install -y tcpdump (없으면)

# SD
#   서버: ./response_sd.sh
#   클라이언트: ./request_sd.sh

# TCP 복구
#   서버: sudo ./response_tcp.sh <클라이언트 IP> 1 8 30
#   클라이언트: ./request_tcp.sh
```

방향을 바꿔 재검증하려면 4)의 `unicast` 덮어쓰기를 서버↔클라이언트 바뀐 쪽으로
다시 맞추고, 실행하는 스크립트만 바꾸면 된다 — 코드/커밋 변경은 전혀 필요 없다.

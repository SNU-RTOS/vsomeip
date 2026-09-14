# vSomeIP 프로토콜 최적화 프로젝트

본 README는 **vSomeIP** 프로토콜의 **Service Discovery (SD) 시간** 및 **TCP 패킷 복구 시간** 최적화를 목표로 진행한 프로젝트의 세부 내용을 담고 있음

---

## 1. 개요

### 프로젝트 배경

[vSomeIP](https://github.com/covesa/vsomeip)는 자동차 통신 시스템에서 널리 사용되는 미들웨어 솔루션으로, ECU 간의 통신을 지원  
본 프로젝트는 **SD 시간**과 **TCP 패킷 복구 시간**의 최적화를 목표

[vSomeIP에 대한 개괄적인 내용](https://github.com/COVESA/vsomeip/wiki/vsomeip-in-10-minutes)

### 프로젝트 목표

1. **SD 지연 시간** 단축

- [x] 2024-2: 4s (검증 통과)
- [ ] 2026-2: 2s

2. **TCP 패킷 복구 시간** 단축

- [x] 2024-2: 4s (검증 통과)
- [ ] 2026-2: 2s


---

## 2. 실험

### 개발 환경

**하드웨어**

- **개발 보드**: NVIDIA Orin 2대
- **CPU**: Armv8
- **메모리**: 32GB

**소프트웨어**

- **운영체제**: Ubuntu 20.04
- **커널 버전**: Linux Kernel 5.10.104-tegra (일부 설정은 제외되어있음)

**네트워크 구성**

- **물리적 네트워크 환경**: NVIDIA Orin 2대를 이더넷 케이블로 다이렉트로 연결하여 통신 테스트.
- **통신 프로토콜**: SD는 UDP의 멀티캐스트, TCP Packet Recovery는 TCP 통신 가정
- **패킷 손실 환경**: 2대가 유선으로 바로 연결되어 있기에 일반적인 상황에서 패킷이 유실될 가능성은 없음. 그렇기에 `tc`, `iptables` 커맨드를 활용하여 인위적으로 손실 유발. 타겟 보드에선 `tc` 커맨드 사용 불가로 `iptables` 커맨드 사용

```bash
sudo iptables -D INPUT -i wlo1 -m statistic --mode random --probability 0.1 -j DROP # 10% 확률로 해당 컴퓨터로 들어오는 패킷의 10%를 유실
```


### 빌드 커맨드

vSomeIP 설치는 다 되었다는 가정 하에 (https://github.com/COVESA/vsomeip)

```
make examples
```

실행으로 examples 내 코드만 빌드 가능

### 테스트 방법

**SD 지연 시간**

1. 한 대의 보드에서 `./response_sd.sh` 스크립트 실행 -> SomeIP 프로토콜의 FindService 호출 시 응답할 수 있게 대기 (이하 server)
2. 다른 보드에서 `./request_sd.sh` 스크립트 실행으로 FindService 송신 (이하 client)
3. server가 client가 전송한 find를 멀티캐스트로 받고 있다가 이를 체크 후 application name, service id, instance id가 일치할 시 응답, 이 때까지 걸린 시간이 SD 지연 시간
4. SD 지연 시간을 10번 측정 한 후 이 평균 값을 1회의 시간으로 측정, 10회의 측정값이 기준값을 통과하는지 최종 확인 (총 100회 요청 테스트)

```bash
./response_sd.sh # 오른쪽 orin
./request_sd.sh # 왼쪽 orin
```


**TCP 패킷 복구 시간**

> 2026-09 기준, 아래의 수동 `iptables`+Wireshark 방식은 [tcp-recovery/](tcp-recovery/)의 도구로
> 대체됐다. 기존 방식의 `-D`(삭제 옵션이라 유실이 주입되지 않음)·`OUTPUT` DROP(TCP가 "안 보낸 것"으로
> 처리해 재전송 타이머를 타지 않음)·캡처 위치 문제를 [tcp-recovery/README.md](tcp-recovery/README.md)
> "왜 새로 만들었나"에 정리해 뒀다. 아래는 과거 기록이자, 지금도 유효한 절차의 개요.

SD 측정(`response_sd.sh`/`request_sd.sh`)과 같은 방식으로, 두 보드에서 스크립트 하나씩만 실행한다 - 둘
사이에 SSH가 필요 없다. `request_tcp.sh` 쪽 화면에 뜨는 `패킷 유실 복구 시간: Xus` 줄이 `request-sd.cpp`의
"매칭까지 처리 시간"과 같은 방식으로 **클라이언트 자신이 직접 잰** 값이다.

```bash
./response_tcp.sh # 서버 - sudo로 실행, 유실 주입도 여기서 관리
./request_tcp.sh  # 클라이언트
```

1. `./response_tcp.sh` 스크립트를 서버에서 sudo로 실행 → 이벤트 발행 시작, 유실 주입도 같이 관리
2. 클라이언트에서 `./request_tcp.sh` 실행 → 구독 시작
3. 패킷 손실이 발생 시 TCP 프로토콜 내에 내장된 로직으로 패킷 손실 복구 과정 진행
4. 손실 후 수신 간격이 정상 주기(최근 수신 간격에서 자동으로 계산됨)의 3배(기본값)를 넘으면 클라이언트가
   직접 "패킷 유실 복구 시간"을 로그로 출력
5. 10건(기본값) 감지하면 클라이언트가 스스로 멈추고 `총 수신 N건 중 M건 유실, 평균 복구 시간 Xus`를 출력

서버 쪽 tcpdump 캡처 하나로 두 보드의 시계 동기화 없이 와이어 레벨로 교차검증하려면
`tcp-recovery/run_experiment.sh`(SSH 자동화) 또는 `tcp-recovery/inject_loss.sh`+`analyze_recovery.py`를
직접 조합해서 쓴다 - 두 방법의 차이와 사용법은 tcp-recovery/README.md 참고.

### 테스트 결과

| **지표**                  | **최적화 전** | **1차 최적화** | **2차 최적화 (Wi-Fi, 2026-09)** | **3차 최적화 (유선 직결, 2026-09)** |
|--------------------------|---------------|---------------|---------------------------|---------------------------------|
| 서비스 디스커버리 시간   | 평균 20ms    | 평균 4ms      | 평균 2.5~3ms (Wi-Fi 링크가 병목) | **중앙값 1.19ms** (100회 중 99회 1.5ms 이내) |
| TCP 패킷 복구 시간       | 평균 10ms    | 평균 4ms      | 평균 3.46ms(와이어) / 9.7ms(체감) | **평균 0.58ms**(와이어, `--cycle 1`) / 2.7ms(체감) |

3차는 Thor↔Orin을 스위치 대신 이더넷 케이블로 직결하고, 발행 주기를 `--cycle 5`에서 `--cycle 1`로
바꾼 뒤의 수치 - 와이어 레벨은 이미 목표(2ms)를 여유 있게 통과한다(개별 건 100% ≤2ms). 체감(애플리케이션
레벨) 값은 여전히 와이어 레벨보다 높은데 - 왜 그런지, 두 지표가 왜 따로 있는지는
[CHANGES_THOR.md](CHANGES_THOR.md) "Self-stop, self-calibrating baseline, and finding `--cycle 1`"와
[tcp-recovery/README.md](tcp-recovery/README.md) "결과: 실측값" 참고. 2차(Wi-Fi) 수치는 비교 기준으로
남겨뒀다. 1차 수치("평균 4ms")는 측정 방법의 결함(`OUTPUT` DROP이 실제로는 선로 유실을 흉내내지 못하는
문제 등) 때문에 이후 수치와 직접 비교하기는 어렵다.

### config

- 최상단 `unicast`: vSomeIp 실행하는 host의 ip 입력
- `cyclic_offer_delay`: SD과정에서 server가 멀티캐스트를 통해 본인의 존재를 알리는 과정을 OfferService라고 함. 이 offer를 `repetitions_max`만큼 반복 후 다시 이를 반복하는데 이 주기를 의미
  - 본 프로젝트 테스트 과정은 offer를 받는 게 아닌 FindService에 대한 응답으로 처리가 되는 시간을 보는 것이 우선. 그렇기에 이 주기를 최대한 길게 하여 OfferService 과정이 일어나지 않게 함
- `request_response_delay`: 한 번 request가 왔을 시 response 하기 전까지 줄 delay를 의미. 이 값으 0으로 만듦으로써 2024년 1차 테스트 통과
- `eventgroups`: SOME/IP에서는 필요하지만 본 프로젝트에서는 사용하지 않음

### 유의사항

- ~~`iptables`를 통한 패킷 로스는 소프트웨어 레벨로 구현이 되어있지만 wireshark는 하드웨어 레벨에서 패킷을 관찰하기에 둘 간의 패킷 불일치 발생~~ →
  `tcp-recovery/inject_loss.sh`는 `iptables OUTPUT DROP`이 아니라 dummy 인터페이스로의 정책 라우팅을 쓴다(자세한 이유는
  [tcp-recovery/README.md](tcp-recovery/README.md) 참고). 캡처와 분석을 서버 쪽 tcpdump 하나로 통일해서 두 장비 간
  불일치 문제 자체를 없앴다.
- 데스크탑에서 잘 돌아가던 코드가 orin에서 테스트할 시 안 되던 케이스가 있었기에 script에 보면 코드, config에 일관성이 없는 문제가 있음
  - SD 과정 없이 IP를 고정으로 TCP 통신하는 config, 별도의 tcp application 코드가 동작하지 않음 등
  - (2026-09) `config/vsomeip-tcp-client.json`·`vsomeip-tcp-service.json`의 `unicast`가 이 보드들과 무관한 옛 IP
    (`192.168.196.27`/`.103`)로 남아있던 것도 이 문제의 사례 - SD 설정과 같은 방식으로 Thor의 실제 IP로 맞춰뒀다.
    Orin에서 쓸 때는 SD 설정과 마찬가지로 자기 IP를 로컬(비커밋) 변경으로 덮어써야 한다 ([CHANGES_THOR.md](CHANGES_THOR.md)
    "Per-device unicast" 참고).


---
## 3. 향후 과제

- (2026-09 갱신) TCP 패킷 복구는 무엇을 바꿔야 하는지 찾았다 - `tcp-recovery/` 참고. 2ms까지 남은 구간은
  두 가지로 좁혀졌다: 커널 틱(`CONFIG_HZ=250`, `CONFIG_HZ=1000`으로 재빌드 시 이론상 ~1.8ms 추정이지만
  실물 하드웨어 커널 재빌드라 시도하지 않음)과 Orin이 Wi-Fi라는 점(유선 홉은 0.24ms). 자세한 분해는
  [tcp-recovery/README.md](tcp-recovery/README.md) "2ms까지 남은 구간" 참고.
- config를 통한 시간 단축, TCP 프로토콜 내에서 할 수 있는 테스트는 다 해봤기에 SOME/IP 내용 이해 후 SomeIP를 구현한 vSomeIP 코드 내에서  C++ 코드 최적화 과정 진행 필요


---

## 4. 참고 자료

- [SOME/IP 프로토콜 명세서](https://some-ip.com/standards.shtml)
  - 이 중 SD 관련된 내용 위주로 살펴보기
- [차량 내에서 TCP](https://some-ip.com/papers/2022-11_IEEE-Techday_TCP_and_Automotive_Ethernet.pdf)
  - 어떻게 차량 환경에서 TCP 프로토콜을 최적화 할 지에 대한 가이드
- 기타 웹 자료
  - https://watchout31337.tistory.com/444
  

---

### 비고

2024.12 기준 담당자
- 옥순환 (shock@redwood.snu.ac.kr)
- 이남철 (nclee@redwood.snu.ac.kr)

# TCP 패킷 복구 시간 측정 도구

`response_tcp.sh` / `request_tcp.sh`로 하던 기존 측정 방식(수동 `iptables`, Wireshark로 눈으로 확인)을
대체하는 도구 모음. 목표는 "클라이언트가 패킷 유실을 인식한 시점부터 서버가 재전송한 패킷을 받기까지의
시간"을 자동으로, 재현 가능하게, 두 장비의 시계 동기화 없이 측정하는 것.

- [inject_loss.sh](inject_loss.sh) - 선로 패킷 유실을 흉내내는 도구 (start/stop)
- [analyze_recovery.py](analyze_recovery.py) - 서버 쪽 tcpdump 캡처 하나로 복구 시간을 계산
- [tune_tcp_recovery.sh](tune_tcp_recovery.sh) - 복구 시간을 줄이는 커널 설정 적용
- [run_experiment.sh](run_experiment.sh) - 위 세 가지를 묶어 서버(로컬)+클라이언트(SSH 원격) 실험을 한 번에 실행

세 도구는 독립적으로도 쓸 수 있다. `run_experiment.sh`는 이 프로젝트의 Thor↔Orin 2대 구성을 위한 편의
스크립트일 뿐이다.

## 왜 새로 만들었나

기존 `README.md`의 방법(`sudo iptables ... -D INPUT ... -j DROP`)에는 세 가지 문제가 있었다.

1. **`-D`는 규칙을 삭제하는 옵션이다.** 적힌 그대로 실행하면 애초에 유실이 주입되지 않는다.
2. **`OUTPUT`에서 `DROP`하면 선로 유실이 되지 않는다.** 보낸 쪽 TCP 스택이 전송 실패로 인식해 그
   세그먼트를 "보낸 적 없음"으로 되돌리고 다음 쓰기에서 다시 큐에 넣는다. 실제 선로 유실(세그먼트가
   상대에게 도달하지 못했지만 보낸 쪽은 성공으로 아는 상황)과는 다른 상황이라 재전송 타이머가 정상적으로
   동작하지 않는다.
3. **Wireshark 수동 측정**은 캡처 위치가 netfilter보다 앞이라 실제로 유실된 패킷도 화면에 보이고, 두
   장비의 시계가 다르면 "클라이언트 인식 시점"을 정의할 방법이 없다.

## 핵심 아이디어: 클라이언트 쪽 복구 시간을 서버 캡처 하나로 계산하기

"클라이언트가 유실을 인식한 시점 → 복구 패킷을 받은 시점"은 클라이언트 쪽에서 재야 할 것 같지만, 실제로는
**서버 쪽 캡처 하나로 계산할 수 있다.**

- 클라이언트가 유실을 알아채는 계기가 되는 패킷("트리거", 구멍 바로 다음에 온 세그먼트)과 그 뒤에 오는
  재전송 패킷은 **둘 다 서버 → 클라이언트로 같은 경로를 지난다.**
- 두 패킷의 편도 지연은 (경로가 같으므로) 사실상 같아서 서로 상쇄된다.
- 그래서 `t_클라이언트(재전송 도착) - t_클라이언트(트리거 도착) ≈ t_서버(재전송 송신) - t_서버(트리거 송신)`
  가 성립하고, 우변은 **서버에서 잰 두 송신 시각의 차이**이므로 클라이언트 시계를 볼 필요가 없다.

`analyze_recovery.py`가 계산하는 `[C]`가 바로 이 값이다.

## 사용법

### 1. 빠른 시작 (Thor=서버, Orin=클라이언트, 이 저장소 그대로)

```bash
# Thor에서, TCP 서버(SOME/IP 이벤트 발행자)를 시작
sudo ./tcp-recovery/tune_tcp_recovery.sh <Orin의 IP>   # 선택 사항, 아래 "결과" 참고
sudo ./tcp-recovery/run_experiment.sh test1 stream 5 60 \
    --server-ip <Thor의 IP> --client-ip <Orin의 IP> \
    --client-host <user>@<Orin의 IP> --client-sudo --vsomeip-dir /workspace/vsomeip
```

`--client-sudo`는 원격(Orin)의 vsomeip 체크아웃이 root 소유일 때만 필요하다. SSH 로그인은 키 기반을
권장하며(`ssh-copy-id <client-host>`), 안 되어 있으면 `export SSHPASS=...` 후 `sudo -E`로 실행한다 -
**비밀번호를 파일이나 명령행에 직접 적지 말 것.** 자세한 내용은 [run_experiment.sh](run_experiment.sh)의
상단 주석 참고.

끝나면 결과가 `tcp-recovery/results/test1/`에 남고, `analyze_recovery.py`의 출력이 화면에 바로 찍힌다.
이 디렉터리는 `.gitignore`에 등록되어 있다 - 커밋하지 말 것.

### 2. 직접 조합해서 쓰기 (다른 두 장비, 다른 트래픽 패턴)

```bash
# 1) 서버 실행 (평가하려는 실제 SOME/IP 서버, 또는 notify-sample 같은 예제)

# 2) 유실 주입 시작
sudo ./tcp-recovery/inject_loss.sh start <서버 IP> <클라이언트 IP> PORT=<TCP 포트>

# 3) 캡처하면서 클라이언트 실행
sudo tcpdump -i <인터페이스> -n -tt --time-stamp-precision=micro -w cap.pcap "tcp port <포트>" &
# ... 클라이언트 실행 ...
sudo kill %1

# 4) 유실 주입 해제
sudo ./tcp-recovery/inject_loss.sh stop <서버 IP> <클라이언트 IP>

# 5) 분석
tcpdump -r cap.pcap -n -S -tt --time-stamp-precision=micro > cap.txt
python3 ./tcp-recovery/analyze_recovery.py --capture cap.txt --port <포트>
```

**주의:** `analyze_recovery.py`는 `-S`(절대 시퀀스 번호)와 `-tt`(epoch, 마이크로초) 옵션으로 만든 텍스트
캡처가 필요하다. 상대 시퀀스 번호로는 구멍(hole) 탐지 로직이 의미가 없어진다.

### 트래픽 패턴이 중요하다

**유실 뒤에 다른 세그먼트가 이어서 오지 않으면 클라이언트는 유실을 알 방법이 없다.** 한 번에 하나씩
요청-응답을 주고받는 락스텝(lockstep) 트래픽(예: `request-sample`/`response-sample`)이 정확히 이 경우다
- 응답이 유실되면 클라이언트는 다음에 보낼 것이 없어 SACK을 만들 수 없고, 복구는 전적으로 서버의
재전송 타이머(RTO)에만 의존한다. 실측 218ms.

`run_experiment.sh`의 `stream` 모드(`notify-sample`/`subscribe-sample`, 주기적 이벤트 스트림)처럼
유실 뒤로도 트래픽이 계속 이어지는 패턴이어야 SACK 기반 빠른 재전송이 동작해서 의미 있는 숫자가 나온다.
`rr` 모드(요청-응답)는 경로 점검용으로만 남겨뒀다.

## 결과 해석

```
재전송: SACK 계기 48건, 타이머 계기(RTO/TLP) 0건
  [B] SACK 수신 -> 재전송 송신 (서버 반응 시간)        : n=48 mean=1971us p50=10us ...
  [C] ★ 클라이언트 기준 복구 시간 (요청하신 지표)      : n=48 mean=3414us p50=1510us p90=6354us max=10822us  (<=2ms: 56%)
```

- **[C]가 요청하신 지표다.** "클라이언트가 유실을 인식 → 복구 패킷 수신"에 해당.
- **[B]**는 참고용: 서버가 SACK을 받고 재전송을 내보내기까지 걸린 시간 (서버 자체의 반응 지연, 원인
  분석용).
- **타이머 계기(n건)**: 이 캡처만으로는 시간을 잴 수 없는 재전송(직전 SACK이 캡처에 없음 - RTO/TLP로
  복구됐다는 뜻). `[C]`에는 포함되지 않는다. 이 값이 크면 트래픽 패턴이 락스텝에 가깝다는 신호.

## `tune_tcp_recovery.sh`가 하는 일과 실측 효과

TCP **서버**(재전송을 보내는 쪽) 에서 실행. 두 설정 모두 휘발성(재부팅하면 사라짐)이고 되돌리는 명령을
출력해 준다.

| 설정 | 무엇을 하나 | 왜 |
|---|---|---|
| `ip route ... rto_min 1ms` | 해당 피어로 가는 경로에만 최소 재전송 타임아웃을 낮춤 | 커널 기본값(200ms)은 SACK으로 못 잡는 경우(락스텝 트래픽, 버스트의 마지막 세그먼트)의 하한선. SACK 경로 자체는 이 타이머까지 가지 않으므로 빠르지 않음 |
| `sysctl tcp_reordering=1` | **호스트 전체**의 SACK 재정렬 임계값을 낮춤 | 커널 기본(3)은 순서 뒤바뀜과 진짜 유실을 구분하려고 기다리는 시간. 순서가 안 바뀌는(직결에 가까운) 링크에서는 이 대기가 그대로 지연으로 남음. 순서가 실제로 바뀌는 링크(다중경로, 로밍 등)에서는 쓰지 말 것 |

Thor(서버, 유선) → Orin(클라이언트, Wi-Fi), 5ms 주기 SOME/IP 이벤트, 최초 전송의 1/8을 유실시킨 실측
(`[C]`, 클라이언트 기준 복구 시간):

| 설정 | mean | p50 | n |
|---|---|---|---|
| 기본값 | 6.51ms | 1.90ms | 292 |
| + `rto_min 1ms` | 4.07ms | 2.11ms | 273 |
| + `rto_min 1ms` + `tcp_reordering=1` | **3.46ms** | **1.64ms** | 293 |

## 2ms까지 남은 구간

최선 조건(3.46ms)을 분해하면:

| 구성 요소 | 평균 기여 |
|---|---|
| Wi-Fi 왕복 지연 | 1.47ms |
| 서버가 즉시 반응한 경우 (61%, [B] ≤100us) | ≈0 |
| **서버가 3~10ms 대기 (RACK 재정렬 타이머, 커널 틱 단위)** | **1.61ms** |
| TLP/RTO (`rto_min 1ms`로 이미 억제됨) | 0.28ms |

이 스크립트들로는 더 줄이기 어려운 두 가지가 남는다.

1. **커널 틱이 4ms 단위다 (`CONFIG_HZ=250`, Thor·Orin 공통).** RACK 재정렬 대기의 상당수가 이 틱
   반올림으로 보인다. `CONFIG_HZ=1000`으로 커널을 다시 빌드하면 이론상 약 1.8ms까지 줄어들 것으로
   추정되지만, **실제로 시도하지는 않았다** - 실물 로봇 하드웨어의 커널 재빌드는 되돌리기 어렵고 부팅
   실패 위험이 있어 사용자 승인 없이 진행할 일이 아니라고 판단했다.
2. **Orin이 유선이 아니라 Wi-Fi다.** 서버가 즉시 반응한 경우만 보면 평균 1.42ms인데 대부분 Wi-Fi 왕복
   지연이다. 같은 스위치의 유선 홉은 0.24ms였다. (자세한 내용은 SD 지연 관련 조사 참고.)

## 알려진 한계

- `inject_loss.sh`는 **서버 → 클라이언트** 데이터 유실만 흉내낸다. ACK 유실이나 버스트 유실(연속된 여러
  세그먼트가 한꺼번에 사라지는 경우)은 다루지 않는다.
- 여러 인터페이스가 같은 서브넷에 있는 호스트(이 프로젝트의 Thor처럼 유선+Wi-Fi를 동시에 쓰는 경우)에서는
  `ip route show default`의 순서가 매 호출마다 같다는 보장이 없다. 세 스크립트 모두 **대상 IP로 실제
  가는 경로**(`ip route get <peer>`)를 우선 사용하도록 만들어 뒀지만, 뭔가 이상하면
  `ip route get <peer-ip>`로 실제 인터페이스를 직접 확인할 것.
- `analyze_recovery.py`는 캡처된 파일 하나만 본다. 서버 쪽 tcpdump가 시작되기 전에 일어난 재전송이나,
  캡처 도중 tcpdump가 패킷을 못 따라간 경우(`tcpdump`가 종료 시 출력하는 "N packets dropped by kernel"
  확인 권장)는 반영되지 않는다.

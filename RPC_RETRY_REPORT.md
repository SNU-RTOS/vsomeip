# RPC Timeout+Retry — 재현 가이드 및 결과

이 세션에서 나온 최종 답: **"유실 시점부터 다시 받을 때까지"를 커널 재빌드 없이, 100%
안정적으로 2ms 밑으로 만든 유일한 방법.** TCP 자체의 재전송(여전히 2.6~2.7ms, 해결 안 됨 -
`CHANGES_THOR.md`의 bpftrace 절 참고)도 아니고, 연속 이벤트 스트림에 UDP 사이드채널을
얹는 하이브리드(역시 2.5~2.8ms, 같은 문서의 "Hybrid" 절 참고)도 아니라, **"요청 하나 보내고
짧은 timeout 안에 응답 없으면 클라이언트가 직접 재전송"**하는 구조다.

## 왜 되는지 한 줄로

연속 스트림(TCP든 UDP든)은 "지금 길어진 게 유실인지 그냥 지터인지"를 통계적으로만 판단할
수 있어서, 오탐을 피하려면 수 ms짜리 안전 마진이 필요하다. **요청/응답 하나는 그 마진이
필요 없다** — 평소 왕복 시간(RTT)보다 살짝 큰 timeout만으로 충분히 안전하게 판단된다. 이
프로젝트의 Thor↔Thor 직결 링크에서 그 RTT는 ~300~900us대라, timeout을 1ms 근처로 잡아도
오탐이 거의 없다.

## 전제 조건

- 두 대의 Thor(또는 Thor/Orin 등)가 이더넷으로 직결되어 있고, 양쪽 다 `thor` 브랜치
  `build/examples/{request,response}-rpc-retry`가 빌드되어 있을 것 (`cmake --build .
  --target examples`로 한 번에 됨). IP 설정·`unicast` 로컬 오버라이드 방법은
  [THOR_THOR_DIRECT_LINK_REPORT.md](THOR_THOR_DIRECT_LINK_REPORT.md) "1. 환경 구성"과
  동일하다 - 거기 나온 대로 두 보드 다 준비되어 있으면 추가로 할 게 없다.
- `config/vsomeip-rpc-retry-client.json`의 `unicast`만, 클라이언트로 쓸 보드에서 로컬로
  자기 IP로 덮어쓰면 된다(커밋하지 않는 로컬 변경, 기존 관행과 동일).

## 실행

```bash
# 서버 역할 보드 - sudo 필요(유실 주입에 iptables/ip route)
sudo ./response_rpc_retry.sh <클라이언트 IP> [1/N 유실=8]
#   예: sudo ./response_rpc_retry.sh 10.10.10.2 8

# 클라이언트 역할 보드
./request_rpc_retry.sh [주기ms=5] [timeout_us=1000] [최대재전송=5] [최소복구건수=10]
#   예: ./request_rpc_retry.sh 5 1000 5 20
```

서버는 클라이언트가 끝나면(또는 서버가 직접 종료되면) 알아서 세션을 정리한다 - 둘 다
SSH로 서로를 조종할 필요 없이, 각자 자기 보드에서 스크립트 하나씩만 실행하면 된다.

**timeout을 바꾸기 전에, 유실 없이 평소 RTT부터 확인할 것** (다른 테스트들과 같은 관행):

```bash
NO_DROP=1 sudo ./response_rpc_retry.sh <클라이언트 IP>   # 서버: 유실 주입 끄고 대기
./request_rpc_retry.sh 5 100000 0 0                        # 클라이언트: timeout 넉넉히, 영원히 실행
```

이 상태로 몇 초 띄워두고 Ctrl-C로 끈 뒤, 요약 줄의 "정상 왕복 평균"을 보고 그보다 확실히
큰 값으로 `timeout_us`를 정한다 - 이 프로젝트의 링크에서는 926us(단독 측정 기준, 최대
1163us)였다.

## 결과 읽는 법

클라이언트 화면에 뜨는 줄:

```
[timeout] 1000us 동안 응답 없음 - 1번째 재전송
[recovered] 재전송 후 복구 시간 335us
...
전송 144건(재전송 포함), 응답 123건, 포기 0건, 정상 왕복 평균 316us (n=103), 재전송 후 복구 평균 335us (n=20)
```

- **"재전송 후 복구 시간"**: 재전송을 보낸 시점부터 응답을 받을 때까지 - 코드가 직접 재는 값.
- **"유실 시점부터"** (이 프로젝트가 실제로 보고하는 헤드라인 숫자): 재전송은 timeout이
  이미 다 지나야 나가므로, `timeout_us + 재전송 후 복구 시간`을 더해야 "원래 요청이 나간
  시점(≈유실이 발생한 시점)부터" 실제로 걸린 시간이 나온다. 요약 줄은 "재전송 후" 값만
  찍으므로, 이 덧셈은 로그를 긁어서 직접 계산해야 한다:

```bash
grep '\[recovered\]' <client_log> | grep -oE '[0-9]+us' | grep -oE '[0-9]+' | \
  python3 -c "
import sys, statistics as st
v=[int(x) for x in sys.stdin]
timeout_us = 1000  # 실행할 때 준 값과 맞출 것
total = sorted(timeout_us+x for x in v)
le2 = sum(1 for x in total if x <= 2000)
print(f'n={len(total)} mean={st.mean(total):.0f}us p50={total[len(total)//2]}us '
      f'max={max(total)}us  <=2ms: {le2}/{len(total)} ({100*le2/len(total):.0f}%)')
"
```

- **"포기"**: `--max-retries`(기본 5)번 재전송해도 응답이 없으면 그 요청은 버려진다 -
  이 값이 0이 아니면 timeout이 너무 짧거나 손실률이 너무 높다는 뜻.

## 이 프로젝트 링크에서 실측한 값 (Thor↔Thor 직결, 1-in-8 유실)

| `--timeout-us` | n | 재전송→응답 | **유실 시점부터** | ≤2ms |
|---|---|---|---|---|
| 2000 | 20 | mean 435us | mean 2435us, max 2868us | 0% |
| 1200 | 20 | mean 398us | mean 1598us, p90 1834us, max 2053us | 95% |
| **1000** | 20 | mean 335us | **mean 1335us, p90 1520us, max 1912us** | **100%** |
| **1000** | 50(재확인) | mean 449us | **mean 1449us, p90 1518us, max 1969us** | **100%** |

재시도 포기(5번 재시도해도 실패) 전 구간에서 0건 - 1-in-8 유실률에서는 한 요청이 연속으로
두 번 유실될 확률이 낮아(~1.5%), 거의 항상 1번 재전송으로 끝난다.

## 더 줄여볼 수 있을까

`--timeout-us`를 더 낮추면 평소 RTT에 더 가까워져 오탐(정상 응답을 유실로 오판해 불필요한
재전송) 위험이 커진다 - 낮추기 전에 반드시 위 "유실 없이 평소 RTT 확인" 절차로 현재 링크의
실측 RTT 분포(특히 max)를 다시 보고, 그보다 여유 있게 잡을 것. 이 프로젝트에서는 1000us가
"100% 통과 + 포기 0건"을 동시에 만족하는 지점으로 확인됐다.

## 참고: 같이 나온 다른 두 가지 (참고용, 권장하지 않음)

- **순수 UDP 버전** (`request/response-udp-recovery.cpp`) - 이벤트 스트림 자체를 UDP로
  바꾸고 시퀀스 번호로 즉시(결정론적으로) 유실을 감지. 메커니즘 자체는 빠르지만(300~600us),
  SOME/IP 필드 이벤트라는 전제가 있어야 자연스럽다.
- **하이브리드 버전** (`request/response-hybrid-recovery.cpp`) - TCP 스트림은 그대로
  두고 유실이 "의심될 때" UDP로 사이드채널 복구를 시도. **효과 없음으로 확인됨** - 연속
  스트림의 통계적 판단 마진(~2ms) 자체가 병목이라, 복구 메커니즘을 바꿔도 별 차이가 없다.
  자세한 경위는 `CHANGES_THOR.md` "Hybrid ... and RPC timeout+retry" 절 참고.

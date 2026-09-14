#!/usr/bin/env python3
"""Compute SOME/IP TCP packet-recovery latency from a server-side tcpdump capture.

Definition used here (as specified for this project): packet-recovery time is the
interval, as observed by the CLIENT, from the moment it can first tell a segment is
missing to the moment the retransmitted copy arrives.

That interval cannot be read directly off a server-side capture, because the client's
clock is not synchronised to the server's. But it does not need to be: both the
"trigger" segment (the one that arrives right after the hole and lets the client
notice it) and the retransmission travel the same server -> client path, so their
one-way delays are equal to first order and cancel out of the difference:

    client-perceived recovery
        = t_client(retransmission arrives) - t_client(trigger arrives)
        ~ t_server(retransmission sent)    - t_server(trigger sent)          [C]

which is computable from server-side send timestamps alone. This is the headline
number this script reports. See tcp-recovery/README.md for the full derivation and
its error bound.

Input: a text capture from
    tcpdump -r <pcap> -n -S -tt --time-stamp-precision=micro > capture.txt
The `-S` (absolute sequence numbers) and `-tt` (unix epoch, microsecond) flags are
required - relative sequence numbers make the hole-detection logic below meaningless,
and the timestamp format must be a bare epoch value for the regex to parse it.
"""
import argparse
import json
import re
import statistics as st
import sys

PKT_RE = re.compile(
    r'^([0-9]+\.[0-9]+) IP \S+\.(\d+) > \S+: Flags \[([^\]]+)\]'
    r'(?:, seq (\d+):(\d+))?(?:, ack (\d+))?'
)
SACK_RE = re.compile(r'sack \d+ \{(\d+):(\d+)')


def parse_capture(path, port):
    """Yield (t, is_server_side, flags, seq_start, seq_end, ack, sack_left) per packet."""
    port = str(port)
    with open(path) as f:
        for line in f:
            m = PKT_RE.match(line)
            if not m:
                continue
            t, pkt_port, flags = m.group(1), m.group(2), m.group(3)
            s = int(m.group(4) or 0)
            e = int(m.group(5) or 0)
            ack = int(m.group(6) or 0)
            sk = SACK_RE.search(line)
            yield (float(t), pkt_port == port, flags, s, e, ack,
                   int(sk.group(1)) if sk else None)


def find_retransmissions(rows):
    """Match each retransmission to the SACK that revealed its hole and to the
    trigger segment that produced that SACK.

    A SACK-bearing ACK with cumulative-ack X reports "I have everything up to X, plus
    a block starting past a hole". X is the left edge of the hole. The retransmission
    that fills it is the next server segment sent with seq == X. The "trigger" is the
    server segment whose arrival prompted the client to generate that first SACK for
    hole X - i.e. the send that immediately preceded it in server time.

    Returns three lists of microsecond deltas: server_reaction ([B]: SACK arrival at
    the server capture point -> retransmit sent - this is *reaction* time, not the
    quantity requested, since "arrival at the server" and "sent by the server" are the
    same capture, not two independent clocks), client_recovery ([C]: the headline
    number, trigger sent -> retransmit sent), and a count of retransmissions that had
    no preceding SACK in the capture (timeout-driven: RTO or TLP, not a fast
    retransmit - recovery time for these must come from the drop log instead).
    """
    server_reaction, client_recovery, timer_driven = [], [], 0
    maxend = 0
    sent_at = {}       # seq_start -> t_sent, for segments not yet acked past their end
    first_sack = {}    # cumulative-ack value -> (t_sack, hole_left_edge)
    for t, is_srv, flags, s, e, ack, sack_left in rows:
        if 'S' in flags:  # new connection: sequence space is unrelated to any previous one
            maxend, sent_at, first_sack = 0, {}, {}
            continue
        if is_srv and e > s:
            if e <= maxend:  # a send that doesn't extend the stream = retransmission
                hit = first_sack.pop(s, None)
                if hit is not None:
                    t_sack, hole_left = hit
                    server_reaction.append((t - t_sack) * 1e6)
                    if hole_left in sent_at:
                        client_recovery.append((t - sent_at[hole_left]) * 1e6)
                else:
                    timer_driven += 1
            else:
                maxend = e
                sent_at[s] = t
        elif not is_srv and sack_left is not None and ack not in first_sack:
            first_sack[ack] = (t, sack_left)
    return server_reaction, client_recovery, timer_driven


def summarize(xs):
    if not xs:
        return {"n": 0}
    xs_sorted = sorted(xs)

    def pct(p):
        return xs_sorted[min(len(xs_sorted) - 1, int(p * len(xs_sorted)))]
    return {
        "n": len(xs), "mean": st.mean(xs), "p10": pct(.1), "p50": pct(.5),
        "p90": pct(.9), "min": min(xs), "max": max(xs),
        "le_2ms_pct": 100 * sum(1 for x in xs if x <= 2000) / len(xs),
    }


def fmt(label, s):
    if s["n"] == 0:
        return f"{label}: n=0"
    return (f"{label}: n={s['n']} mean={s['mean']:.0f}us p50={s['p50']:.0f}us "
            f"p90={s['p90']:.0f}us max={s['max']:.0f}us  (<=2ms: {s['le_2ms_pct']:.0f}%)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--capture", required=True,
                     help="tcpdump -r <pcap> -n -S -tt --time-stamp-precision=micro output")
    ap.add_argument("--port", type=int, default=30510, help="reliable SOME/IP TCP port")
    ap.add_argument("--json", action="store_true", help="also print a JSON summary")
    args = ap.parse_args()

    try:
        rows = list(parse_capture(args.capture, args.port))
    except OSError as e:
        print(f"캡처 파일을 열 수 없음: {e}", file=sys.stderr)
        sys.exit(1)
    if not rows:
        print(f"경고: {args.capture}에 TCP 패킷이 없음 (tcpdump -S -tt 옵션으로 만들었는지 확인)",
              file=sys.stderr)
        sys.exit(1)
    if not any(is_srv for _, is_srv, *_ in rows):
        print(f"경고: --port {args.port}에 해당하는 패킷이 없음 - 포트를 확인할 것", file=sys.stderr)
        sys.exit(1)

    reaction, recovery, timer_driven = find_retransmissions(rows)
    sB, sC = summarize(reaction), summarize(recovery)

    print(f"재전송: SACK 계기 {len(reaction)}건, 타이머 계기(RTO/TLP) {timer_driven}건")
    print(f"  [B] SACK 수신 -> 재전송 송신 (서버 반응 시간)        {fmt('', sB)}")
    print(f"  [C] ★ 클라이언트 기준 복구 시간 (요청하신 지표)      {fmt('', sC)}")

    if timer_driven:
        print(f"  (참고: 타이머 계기 재전송 {timer_driven}건은 이 파일만으로는 시간을 잴 수 없음 - "
              f"직전 SACK이 캡처에 없다는 뜻으로, TLP/RTO 등 긴 대기가 있었을 가능성이 높음)")

    out = {"reaction_B": sB, "client_recovery_C": sC, "timer_driven_retransmits": timer_driven}

    if args.json:
        print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()

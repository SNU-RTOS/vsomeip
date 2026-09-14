# vsomeip Thor Branch Changes

## Reproducing These Results

Quick reference for reproducing the current numbers from a clean checkout on both boards. For *why* each piece exists, see the dated sections below — this is just the "run these commands" summary, kept up to date as the defaults change.

**One-time setup (both boards):**

1. Build this branch's `libvsomeip3` and the `examples/` (`request-sd`, `response-sd`, `request-tcp-recovery`, `response-tcp-recovery`) — see the top-level `README.md` build command.
2. Physically connect Thor's `enP2p1s0` to the Orin's `eno1` with an Ethernet cable (no switch in between). Configure a static IP on each via NetworkManager (not `ip addr add` — see "Direct Ethernet Link" below for why):
   ```bash
   sudo nmcli connection add type ethernet ifname <iface> con-name thor-orin-direct \
       ipv4.method manual ipv4.addresses <10.10.10.1 on Thor, .2 on Orin>/24 ipv6.method ignore
   sudo nmcli connection modify "Wired connection 1" autoconnect no   # old DHCP profile, if present
   ```
3. Thor's JSON configs (`config/vsomeip-{udp,tcp}-{client,service}.json`) are tracked with `unicast: 10.10.10.1`. On the Orin, override `unicast` to `10.10.10.2` in all four — **as an uncommitted local change**, never commit the Orin's addresses over Thor's.
4. Apply host tuning on both, **once per reboot** — not before every individual test run. Both scripts set volatile host state (governor, cpuidle, EEE, a route's `rto_min`, a sysctl) that stays in effect until the next reboot regardless of how many SD/TCP runs happen in between, and there's no ordering requirement against `request_sd.sh`/`response_sd.sh`/`request_tcp.sh`/`response_tcp.sh` either:
   ```bash
   sudo ./tune-all.sh <peer-ip>
   ```
   `tune-all.sh` is `tune-latency.sh` (host: cc7 idle off, governor=performance, EEE off — helps SD and TCP alike) plus `tcp-recovery/tune_tcp_recovery.sh` (TCP-only: `rto_min` 1 ms + `tcp_reordering=1`) in one call, both driven off `<peer-ip>` so the interface is derived from the actual route to the peer — pass the peer IP even for SD-only use, since on this project's gateway-less direct link (no default route) that's the only reliable way to find the right interface at all; without it, `tune-latency.sh` alone silently tunes whatever interface *does* have a default route (typically Wi-Fi) instead. The two underlying scripts still work standalone if you only want one of them.

**SD (server = either board, client = the other — see "Role Swap Compatibility" below):**

```bash
./response_sd.sh          # on the server
bash request_sd.sh        # on the client — 100 runs, prints per-run time + its own (fragile, see
                           # below) average; cross-check with your own min/p50/p90/max from the
                           # "실행 횟수: N, 처리 시간: X us" lines if the printed average looks off
```

Expect median ~1.2 ms, occasional outliers up to a few ms, rarely (seen once in ~200 runs) a multi-hundred-ms one tied to a lost FindService needing a full retry cycle — not reproduced on retry, not chased further.

**TCP packet recovery, application-level (primary method — client's own log, no SSH needed):**

```bash
sudo ./response_tcp.sh <client-ip>     # on the server — starts loss injection too
./request_tcp.sh                       # on the client — stops itself after 10 flagged events
```

Prints its own summary and exits: `총 수신 N건 중 M건 유실, 평균 복구 시간 Xus`, expect mean ~2.7 ms (defaults: `--cycle 1 --threshold 3.0`). The "normal gap" baseline is self-calibrating (median of recent unflagged gaps, `--cycle` only seeds it) — a `--cycle` that doesn't match the server is a slower warm-up now, not a correctness bug, but `--threshold` is still worth a `NO_DROP=1 sudo ./response_tcp.sh <client-ip>` sanity check if you change `--cycle` a lot (see `tcp-recovery/README.md`).

**TCP packet recovery, wire-level (cross-check — needs SSH between the boards):**

```bash
sudo -E ./tcp-recovery/run_experiment.sh <label> stream 1 40 \
    --server-ip <server-ip> --client-ip <client-ip> --client-host <user>@<client-ip> --client-sudo
```

Expect mean ~0.58 ms, 100% of individual events ≤2 ms (`--cycle 1` — this is what the application-level number above is checked against, and where most of the gap between the two comes from; see "Self-stop, self-calibrating baseline, and finding `--cycle 1`" below). **Use `--cycle 1`, not a larger one** — the `--cycle 5` this project used earlier gives a mean several times worse for reasons unrelated to loss (kernel-tick-quantized RACK waits), not a useful comparison point.

**Common gotchas when re-running any of the above** (all encountered and fixed in this document's history, listed here since they'll happen again):
- A prior run's server process still holds the TCP port → `bind failed (Address already in use)`. Check `sudo ss -ltnp | grep 30510` and kill it before restarting.
- Leftover `/tmp/vsomeip-0` / `/tmp/vsomeip.lck` from a `kill -9`'d process → `rm -f` them (as the user that owned them, or `sudo`).
- Leftover `inject_loss.sh` rules from an interrupted run → `sudo ./tcp-recovery/inject_loss.sh stop <server-ip> <client-ip>` is idempotent, safe to run even if nothing is active.
- `set -e` scripts (`response_tcp.sh`, `run_experiment.sh`) that background a long-lived process and get killed with `kill -9` skip their own `trap cleanup EXIT` — the three points above are exactly what that cleanup would otherwise have handled.

---

## Goal

Reduce service discovery (SD) latency between the AGX Thor and the AGX Orin to **2 ms**, measured by `examples/request-sd.cpp` as the time from the client's `ST_REGISTERED` state callback to its `on_availability` callback for service `0x1234.0x5678` over UDP.

---

## Device Role Assignment

Either device can take either role. The same commit, scripts and SD parameters are used on both; only the `unicast` address in the JSON files differs per device (verified 2026-09-14, see [Role Swap Compatibility](#role-swap-compatibility)).

| Role | Script | Binary | Config |
|---|---|---|---|
| **Server** — start first | `response_sd.sh` | `build/examples/response-sd` | `config/vsomeip-udp-service.json` |
| **Client** | `request_sd.sh` | `build/examples/request-sd` | `config/vsomeip-udp-client.json` |

| Device | Address (`unicast` in both JSON files) | Interface |
|---|---|---|
| AGX Thor | `192.168.196.246` | `enP2p1s0` (wired GbE) |
| AGX Orin | `192.168.196.122` | `wlP1p1s0` (5 GHz Wi-Fi — `eno1` has no cable) |

Starting the server before the client ensures the server is already in its main (cyclic offer) phase when the client boots, so the client's very first FindService gets an immediate unicast reply. This eliminates the `repetitions_base_delay` retry wait entirely.

Do not run a second server with the same service/instance ID on the LAN while benchmarking — it silently stops the other server's offers from being answered.

---

## Changes

### 1. Build fix — suppress false-positive GCC warning (`CMakeLists.txt`)

GCC's `-Wstringop-overflow` produces a false positive inside Boost ICL headers (`boost::icl::interval_set`) used by vsomeip's security policy code. With `-Werror` enabled this aborts the build.

**Fix:** Added `-Wno-error=stringop-overflow` to `OS_CXX_FLAGS` in `CMakeLists.txt` (line 82). This demotes the false positive from fatal to non-fatal while keeping `-Werror` active for all other warnings.

---

### 2. Zero out SD initial delay — server config (`config/vsomeip-udp-service.json`)

```diff
- "initial_delay_min" : "1",
- "initial_delay_max" : "5",
+ "initial_delay_min" : "0",
+ "initial_delay_max" : "0",
```

**Why:** On startup, vsomeip draws a random delay in `[initial_delay_min, initial_delay_max]` ms before sending the first proactive OfferService broadcast. The original 1–5 ms range adds unnecessary startup latency before the server is discoverable.

---

### 3. Zero out SD initial delay — client config (`config/vsomeip-udp-client.json`)

```diff
- "initial_delay_min" : "1",
- "initial_delay_max" : "2",
+ "initial_delay_min" : "0",
+ "initial_delay_max" : "0",
```

**Why:** The same initial delay applies to the client's find debounce timer (`start_find_debounce_timer`). The client waits this random interval before firing its first FindService. Setting it to 0 makes the FindService go out immediately on application start.

---

### 4. Always respond with unicast OfferService (`implementation/service_discovery/src/service_discovery_impl.cpp`)

```diff
 void service_discovery_impl::send_uni_or_multicast_offerservice(
         const std::shared_ptr<const serviceinfo> &_info, bool _unicast_flag) {
-    if (_unicast_flag) { // SID_SD_826
-        if (last_offer_shorter_half_offer_delay_ago()) { // SIP_SD_89
-            send_unicast_offer_service(_info);
-        } else { // SIP_SD_90
-            send_multicast_offer_service(_info);
-        }
-    } else { // SID_SD_826
-        send_unicast_offer_service(_info);
-    }
+    (void)_unicast_flag;
+    // Always respond unicast: eliminates multicast join/processing latency
+    // for the two-device (Thor↔Orin) point-to-point setup.
+    send_unicast_offer_service(_info);
 }
```

**Why:** Per the SOME/IP-SD spec (SIP_SD_90), when a server receives a FindService on its unicast address but has not offered recently, it is supposed to respond via multicast so all listeners are informed simultaneously. In a two-device point-to-point setup this is counterproductive — the multicast path incurs OS-level multicast join and packet processing overhead compared to a direct unicast reply. Forcing unicast always yields the lowest possible response latency.

**Verified:** packet captures in both role assignments show every OfferService leaving the server as unicast to the client.

---

### 5. Start SD without waiting for netlink — opt-in (`implementation/routing/src/routing_manager_impl.cpp`, commit `931905d`)

Added to `routing_manager_impl::start()`, between `stub_->start()` and `host_->on_state(ST_REGISTERED)`:

```diff
     if (stub_)
         stub_->start();
+
+#if defined(__linux__) || defined(ANDROID)
+    if (const char *its_fast_start = getenv("VSOMEIP_SD_FAST_START")) {
+        if (its_fast_start[0] == '1') {
+            std::lock_guard<std::mutex> its_lock(pending_sd_offers_mutex_);
+            if (!routing_running_) {
+                if_state_running_ = true;
+                sd_route_set_ = true;
+                start_ip_routing();
+            }
+        }
+    }
+#endif
+
     host_->on_state(state_type_e::ST_REGISTERED);
```

**Why:** On Linux, `start()` launches an asynchronous netlink dump and then immediately reports `ST_REGISTERED`. Service discovery only starts once netlink reports both the interface up (`if_state_running_`) and the route present (`sd_route_set_`), via `on_net_interface_or_route_state_changed()` → `start_ip_routing()` → `discovery_->start()`. A `request_service()` issued from the state handler therefore sits idle until SD comes up:

| Segment (client, debug-log timestamps) | Measured |
|---|---|
| `ST_REGISTERED` → `service_discovery_impl::start()` | 330–776 µs |
| `service_discovery_impl::start()` → FindService handed to `send()` | ~50 µs |

With `VSOMEIP_SD_FAST_START=1`, IP routing and SD start before `ST_REGISTERED` is reported. The later netlink callback hits its `!routing_running_` guard, so SD is never started twice. Without the variable, behaviour is identical to upstream.

**Measured (12 runs each):**

| Setup | Off, median | On, median |
|---|---|---|
| Client and server on the same host (no network) | 1349 µs | 1116 µs |
| Thor client ↔ Orin server over Wi-Fi, Orin power save off | 2724 µs | 2488 µs |

**Limitation:** only use it when the interface bound to `unicast` is already up when the application starts. If the link is down at startup, netlink will not restart SD later because `routing_running_` is already set.

---

### 6. Enable fast SD start in the client script (`request_sd.sh`)

```diff
 export VSOMEIP_APPLICATION_NAME="client-sample"
+# ST_REGISTERED 직후 netlink 응답을 기다리지 않고 SD 시작 (routing_manager_impl::start 참고)
+# 끄고 비교하려면: VSOMEIP_SD_FAST_START=0 ./request_sd.sh
+export VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"
 ...
-for i in {1..10}
+for i in {1..100}
```

**Why:** change 5 is opt-in, and the scripts previously never set the variable, so benchmarks run through `request_sd.sh` did not use it. It now defaults to on and can still be overridden for A/B runs (`VSOMEIP_SD_FAST_START=0 ./request_sd.sh`). Verified that the spawned `request-sd` process receives `1` by default and `0` when overridden.

The run count goes from 10 to 100. Over Wi-Fi, a single 16–60 ms outlier moves a 10-run mean by 1–6 ms — in the role-swap run, one 16 ms outlier put the mean at 4358 µs against a median of 3118 µs. One pass now takes about 200 s, since each run is given 2 s.

---

### 7. Host latency tuning script (`tune-latency.sh`)

Run with `sudo` on **both** devices before benchmarking, regardless of role. Every setting is volatile and is lost on reboot. Optional argument: interface name. Default: the interface holding the default route.

| Lines | Controls | Why | Measured |
|---|---|---|---|
| 8–9 | `set -e`, root check | fail fast instead of a stream of `Permission denied` | — |
| 12 | interface auto-detect | a hardcoded `enP2p1s0` silently skipped the NIC step on the Orin | — |
| 16 | `cpuidle/state1/disable = 1` | block the `cc7` idle state (10,000 µs advertised exit latency) | Thor→Orin ICMP 1.78 → 1.57 ms |
| 19 | `scaling_governor = performance` | `schedutil` let cores idle at 972 MHz | 1.57 → 1.52 ms |
| 22–23 | `nvpmodel -m 0`, `jetson_clocks` | max power mode, pinned clocks | not measured separately |
| 27–29 | Wi-Fi `power_save off` | the AP buffers frames for a sleeping station | **Orin ICMP avg 5.47 → 1.73 ms** |
| 32 | wired EEE off | keep the PHY out of Low Power Idle | not separable from noise; applying it renegotiates the link (~2 s outage) |
| 33 | interrupt coalescing `rx-usecs 0` | lower RX interrupt delay | Thor's driver refuses it |

Verified on Thor (wired branch): refuses to run without root, detects `enP2p1s0`, and leaves governor `performance`, `cc7` disabled and EEE disabled. On Thor, `jetson_clocks` also enables GPU persistence mode. Verified on the Orin (Wi-Fi branch): leaves `wlP1p1s0` power save off, governor `performance` and `cc7` disabled.

---

## Measured Latency Budget

Client on Thor and server on Orin; `tcpdump` on Thor correlated with the client's epoch timestamps:

| Segment | Cost |
|---|---|
| `ST_REGISTERED` → FindService on the wire | ~630 µs (~50 µs once SD is running; the rest is the netlink wait removed by change 5) |
| **FindService out → OfferService in (Wi-Fi round trip + Orin processing)** | **1750–2230 µs** |
| OfferService in → `on_availability` (dispatcher thread hop) | 130–250 µs |

**Root cause of the remaining latency: the Orin is on Wi-Fi.** ICMP round trip from Thor is 1.5–1.9 ms to the Orin, against 0.24 ms to a wired host on the same switch. After all tuning, the wire round trip stayed at 1570–2910 µs (mean 2040 µs). CPU and vsomeip settings cannot reach that. With both ends on the same host (no network), the software floor measured 700–1400 µs.

Connecting the Orin's `eno1` should bring the total near 1 ms. After doing so, update `unicast` in **both** Orin JSON files to the `eno1` address. Otherwise vsomeip keeps binding to the Wi-Fi address.

### Tested and rejected (no measurable effect; not committed)

- `io_thread_nice: -20` per application — `nice()` fails without privileges and gains nothing with them
- running the client under `chrt -f 80` (SCHED_FIFO)
- disabling file and DLT logging — none of it runs inside the measured window

### Recommended, not yet applied

- client `repetitions_base_delay` 200 → 20 ms. About 1 run in 10 loses the multicast FindService and pays the full retry delay; 20 ms cut an observed 200 ms outlier to 63 ms.
- `find_debounce_time_` is hardcoded at `VSOMEIP_SD_DEFAULT_FIND_DEBOUNCE_TIME` (500 ms) and cannot be configured. A service requested after SD has already started waits for the next 500 ms tick.

---

## Latency Sources Eliminated

| Source | Location | Before | After |
|---|---|---|---|
| Client find debounce | `start_find_debounce_timer` | 1–2 ms random | 0 ms |
| Server offer debounce | `start_offer_debounce_timer` | 1–5 ms random | 0 ms |
| Server SD response path | `send_uni_or_multicast_offerservice` | unicast or multicast | unicast always |
| Build failure | `CMakeLists.txt` | `-Werror` on Boost ICL false positive | suppressed |
| SD start after registration | `routing_manager_impl::start` | 330–776 µs netlink wait | skipped with `VSOMEIP_SD_FAST_START=1` |
| Orin Wi-Fi power save | `tune-latency.sh` | ICMP avg 5.47 ms | 1.73 ms |
| CPU idle / frequency scaling | `tune-latency.sh` | `cc7` + `schedutil` | disabled / `performance` |

---

## Role Swap Compatibility

Checked on 2026-09-14 by running Thor as server and Orin as client with the unmodified `response_sd.sh` / `request_sd.sh`. Both devices had been rebooted, so no tuning was active, and the Orin did not yet have change 5:

- 10/10 runs discovered the service: 2368–3831 µs, median 3118 µs, plus one 16,371 µs outlier with no FindService retransmission
- every OfferService left Thor as unicast; Thor's FindService-in → OfferService-out time was 346–922 µs (mean 726 µs)

| Item | Role-independent? |
|---|---|
| JSON files | yes — each device's client and service files both carry its own `unicast`, and all SD parameters match |
| `request_sd.sh`, `response_sd.sh` | yes — identical on both devices |
| Change 4 (unicast offer) | yes — needed by whichever device is server |
| Change 5 (fast SD start) | yes, once built on both — benefits whichever device is client |
| `tune-latency.sh` | yes — run on both |

**Deployment:** both devices must run a `libvsomeip3` rebuilt from change 5 onward. With an older library, `VSOMEIP_SD_FAST_START` is silently ignored on whichever device is the client. As of 2026-09-14, Thor and Orin are both rebuilt from `931905d` and tuned with `tune-latency.sh`.

**Per-device `unicast`:** the tracked JSON files carry Thor's address. The Orin keeps `192.168.196.122` as an uncommitted local change — do not commit the JSON files from the Orin.

## `request_sd.sh`'s printed average can be garbage — bug, fixed

Found while re-checking the ~501 ms outlier below for reproducibility (a full second 100-run pass, not reproduced — max was 5.3 ms that time). The re-run's own printed `평균 처리 시간:` line came out as `5342` followed by a wall of stray numbers on their own lines — not a valid average at all, even though every individual `실행 횟수: N, 처리 시간: X us` line that run printed was fine (independently verified: n=100, min 779 µs, p50 1386 µs, max 5342 µs — no outlier).

Cause: `request-sd`'s `on_availability` occasionally logs "매칭까지 처리 시간" more than once in a single run (seen with duplicate/retransmitted OfferService replies). `TIME=$(grep '매칭까지 처리 시간' temp_output.txt | ...)` had no `-m1`, so on those runs `$TIME` became multiple newline-separated numbers, and feeding that into `total_time=$(echo "$total_time + $TIME" | bc)` corrupted the running total for the rest of the script — hence the accumulated `평균 처리 시간:` at the end being nonsense, while each individual per-run print (computed independently, before accumulation) stayed correct.

Fixed: `grep -m1` (first match only) plus a `[[ $TIME =~ ^[0-9]+$ ]]` guard before accumulating, so a malformed extraction is skipped (and `count` stays accurate) instead of corrupting `total_time`. Per-run lines were never wrong; only trust the script's own final average after this fix — before it, always cross-check with your own `grep -oE '처리 시간: [0-9]+' | ...` over the per-run lines, which is what every number in this document actually came from anyway.

---

# Direct Ethernet Link (Thor↔Orin)

## What changed

The LAN cable was moved off the shared switch (where both boards previously got `192.168.196.0/24` addresses via DHCP) to a direct connection between Thor's `enP2p1s0` and the Orin's `eno1` — a dedicated point-to-point 5 Gbps link, no switch, no gateway. Both boards keep their existing Wi-Fi connections for everything else (SSH, internet).

**Addressing:** `10.10.10.1` (Thor, tracked in the SD/TCP JSON configs) / `10.10.10.2` (Orin, uncommitted local override — same convention as before). Configured via NetworkManager static profiles, not a bare `ip addr add`:

```bash
sudo nmcli connection add type ethernet ifname <iface> con-name thor-orin-direct \
    ipv4.method manual ipv4.addresses <address>/24 ipv6.method ignore
sudo nmcli connection modify "Wired connection 1" autoconnect no   # the old DHCP profile
```

A bare `ip addr add` was tried first and got silently evicted mid-session — NetworkManager still watches the interface's carrier state even with no active profile, and a link renegotiation (in this case, `tune-latency.sh`'s `ethtool --set-eee`) was enough to make it try to reclaim the interface via DHCP, which fails on this link and leaves it addressless. The `nmcli` profile above is what actually stuck, and survives reboots.

## SD start was completely broken on this link, not just slower

`response_sd.sh` (the server) never set `VSOMEIP_SD_FAST_START` — only the client side (`request_sd.sh`) had it, from the earlier SD work. On the switched network this only cost ~700 µs (the netlink wait). On this link it is fatal: `routing_manager_impl::start()` waits for a netlink **default-route** event before starting SD, and a bare point-to-point `/24` with no gateway never produces one. The server sat there re-printing its version banner every 10 s, forever — `discovery_->start()` was never called, the SD multicast group was never joined, nothing was listening on port 30490. Symptom: the client logs "Network interface ... state changed: up" and then nothing else, no "Service is available" ever.

Fixed by adding the same `VSOMEIP_SD_FAST_START="${VSOMEIP_SD_FAST_START:-1}"` default to `response_sd.sh`, `response_tcp.sh`, `request_tcp.sh`, and `tcp-recovery/run_experiment.sh`'s server and client launches (`request_sd.sh` already had it). **On this kind of link the variable is required for SD to work at all, not just an optimization** — worth remembering if this link is ever reused for another test script that doesn't source it.

## Results

**SD (`request_sd.sh`, 100 runs, `tune-latency.sh` applied on both, no kernel changes):**

| | n | mean | p50 | p90 | max |
|---|---|---|---|---|---|
| Direct link | 99 (1 excluded) | 1193 µs | 1188 µs | 1274 µs | 1482 µs |
| — outlier | 1 | 500,924 µs | — | — | — |
| Wi-Fi (previous) | ~100 | ~3100 µs | — | — | — |

99/100 runs landed under 1.5 ms — comfortably under the 2 ms target. The one ~501 ms outlier (run 55 of 100) looked like a single dropped/delayed FindService needing a full `repetitions_base_delay` retry cycle; **re-ran the full 100 a second time and it didn't recur** (that pass: n=100, min 779 µs, p50 1386 µs, max 5342 µs, no outlier at all) — treating it as a rare, non-systemic fluke rather than something to chase further. That second run also surfaced an unrelated bug in the test script itself, fixed separately above ("`request_sd.sh`'s printed average can be garbage").

**TCP packet recovery, wire-level (`tcp-recovery/analyze_recovery.py`'s `[C]`, `--cycle 5`, 1-in-8 loss, 40 s):**

| | n | mean | p50 | p90 | ≤2ms |
|---|---|---|---|---|---|
| Direct link, untuned | 45 | 2700 µs | 574 µs | 5629 µs | 58% |
| Direct link, + `tune_tcp_recovery.sh` | 47 | 2393 µs | 569 µs | 5625 µs | 64% |
| Wi-Fi, + `tune_tcp_recovery.sh` (previous) | 293 | 3460 µs | 1640 µs | 6345 µs | 55% |

The median more than tripled in improvement (1.64 ms → 0.57 ms) just from the link; kernel tuning still helps a bit on top of it, for the same reason as before (`rto_min`/`tcp_reordering`), but the p90/max are still pinned around 5.6 ms regardless of tuning — consistent with the earlier finding that this tail is `CONFIG_HZ=250`-quantized RACK reorder waits, which only a kernel rebuild would touch (see "What still stands between 3.46 ms and 2 ms" below — the numbers there are superseded by this section for the wire-level case, kept for the Wi-Fi comparison).

**TCP packet recovery, application-level (`request_tcp.sh`'s own "패킷 유실 복구 시간" log):**

| | n | mean | note |
|---|---|---|---|
| Direct link, untuned | 12 | 7432 µs | tight cluster, 10/12 within 7744–7808 µs |
| Direct link, + `tune_tcp_recovery.sh` | 11 | 7782 µs | no change — see below |
| Wi-Fi, untuned (previous) | 21 | 9737 µs | — |

Barely moved despite the link and kernel tuning both improving the wire-level number substantially. The tight, near-identical clustering both before and after tuning (and regardless of link) pointed at something in vsomeip's own TCP send pacing (`wait_until_sent`), independent of both the network and the kernel's retransmission timers.

**Follow-up: this was the `--cycle` (publish interval) all along, and it's fixed.** Swept `--cycle` at 1/5/20/50 ms with `NO_DROP=1` (same direct link): achieved throughput was **14.2–15.2% of nominal at every one of them** — 2831/20000, 594/4000, 151/1000, 61/400 respectively. Not a fixed absolute ceiling (throughput did scale with the requested rate) and not fixable by asking for a *slower* rate either — the efficiency is pinned at ~15% regardless, so the actual inter-send interval is always ~6.6–7.1× whatever `--cycle` requests. Earlier guidance in `tcp-recovery/README.md` to "use a longer cycle" was a reasonable-sounding hypothesis that this sweep disproves.

The fix runs the other way: since the real interval scales with the requested one, asking for a *shorter* cycle shortens the real interval too, which is what actually matters for this measurement (a tight real interval means the next event — the "trigger" a loss needs to be noticed — arrives soon after the loss). Re-ran with `--cycle 1 --threshold 3.0` (the 3× threshold matches the ~7 ms real interval at this cycle, checked against a `NO_DROP=1` baseline: 0/2828 false positives): mean dropped from 7.43/7.78 ms (old `--cycle 5` default, untuned/tuned) to **2.73 ms** (n=71) and **2.76 ms** (n=81, repeat run) — line up with the wire-level 2.39–2.70 ms almost exactly. `request_tcp.sh`/`response_tcp.sh` now default to `--cycle 1 --threshold 3.0`.

**Follow-up 2: the earlier `wait_until_sent`/blocking-caller theory above is wrong — disproved by direct instrumentation, not just re-guessed.** Added temporary timing around `app_->notify()` and around the full publish-loop iteration in `response-tcp-recovery.cpp` (`--cycle 1`, rebuilt, run and reverted — not committed). Result: **both stay at almost exactly the requested 1 ms, every iteration, for 2500+ consecutive calls, with or without a client connected.** `notify()` itself never took more than a few µs. So the publish loop is not stalled, blocked, or throttled by anything — it keeps calling `notify()` at full speed the whole time. The `wait_until_sent: Maximum wait time for send operation exceeded` line quoted above is real, but it is a rare *timeout* event (10 s ceiling, `VSOMEIP_MAX_TCP_SENT_WAIT_TIME`) that fired once near a run's end — not a continuous throttle explaining an ~85% loss throughout.

That leaves the ~15% ceiling's actual location unidentified, past what a caller-side instrumentation can see — somewhere between the (confirmed-uncongested) call to `notify()` and the client's socket: vsomeip's per-connection send queue (`tcp_server_endpoint_impl`'s `queue_`, apparently unbounded by default — `check_queue_limit`'s cap was never hit, no "queue size limit reached" logged), the `boost::asio::async_write` completion chain, or the receive/dispatch side on the client. Whichever it is, it does not depend on link speed (same ~15% over Wi-Fi and over this 5 Gbps direct link) or on the kernel tuning applied elsewhere in this document. Not chased further — would mean reading vsomeip's internal queue/io_context code, not host or network configuration, and the practical workaround (shorter `--cycle`, above) already closes the gap to the wire-level number for this project's purposes.

## What this doesn't change

CPU/idle tuning (`tune-latency.sh`) and TCP kernel tuning (`tune_tcp_recovery.sh`) are still volatile — reset on reboot, re-apply with `sudo ./tune-latency.sh <iface>` / `sudo ./tcp-recovery/tune_tcp_recovery.sh <peer-ip>` (interface/peer auto-detected correctly on this link, since both scripts resolve via the actual route to the peer, not the default route — see the SD section's "여러 인터페이스" caveat). The NetworkManager static-IP profiles above are the one persistent change from this section.

---

# TCP Packet Recovery

## Goal

Reduce TCP packet-recovery time — defined as the interval, as observed by the **client**, from being able to tell a segment is missing to receiving the retransmitted copy — to **2 ms**, measured between Thor and the Orin over their reliable SOME/IP TCP endpoint (port 30510).

## Why the previous method didn't work

The method recorded in `README.md` (`sudo iptables -D INPUT -i wlo1 ... -j DROP`) had three problems, found while trying to reproduce it (2026-09-14):

1. `-D` deletes a rule; it never injects loss as written.
2. Even fixed to `-I`/`-A`, dropping on `OUTPUT` doesn't emulate wire loss: the sender's own TCP stack sees a send error and re-queues the segment as "never sent" instead of it looking like a genuinely lost segment, so the retransmission timers under test never fire correctly. Measured effect of this mistake: 0 kernel retransmits recorded; recovery deferred entirely to the *application's* request retry (hundreds of ms).
3. Reading recovery time off Wireshark on two unsynchronized clocks doesn't define "the client noticed the loss" for lockstep traffic, and the two boards' captures disagree about what a software-level `iptables` drop even removed (the note already in this file's "유의사항" section).

## New tooling: `tcp-recovery/`

Replaces the manual `iptables`+Wireshark workflow. Full usage and result tables in [tcp-recovery/README.md](tcp-recovery/README.md); summary:

- `tcp-recovery/inject_loss.sh` — emulates wire loss for 1-in-N first-transmissions of the server's TCP segments, by fwmark + policy routing into a dummy interface (which reports `NETDEV_TX_OK`, unlike an `iptables DROP`) instead of the real one.
- `tcp-recovery/analyze_recovery.py` — computes recovery time from a **single server-side** `tcpdump -S -tt` capture. Key insight: the segment that lets the client notice a hole (the "trigger") and the retransmission that fills it both travel server→client over the same path, so their one-way delays cancel; `t_server(retransmit sent) - t_server(trigger sent)` equals the client-perceived recovery interval without needing clock sync between the two boards. This is the `[C]` metric it reports.
- `tcp-recovery/tune_tcp_recovery.sh` — applies the two kernel settings found to help (below).
- `tcp-recovery/run_experiment.sh` — orchestrates server (local) + client (SSH remote) + loss injection + capture + analysis for this project's two-board setup. Takes the SSH password only via the `SSHPASS` env var (never as literal text in a command or file) and feeds it to a remote `sudo` prompt through the SSH session's own stdin pipe, not as argv text, so it never appears in `ps` output on either host.

Also required: `config/vsomeip-tcp-client.json` and `vsomeip-tcp-service.json` carried stale `unicast` addresses (`192.168.196.27` / `.103`) unrelated to either board, left over from an earlier lab setup — the server was advertising an address that doesn't exist on this host, so the client's TCP handshake went nowhere and no traffic ever reached the interface being captured. Fixed both to Thor's address (`192.168.196.246`), matching the SD JSON convention: the Orin overrides `unicast` locally to its own address, uncommitted, same as for the SD configs.

### Per-device scripts with direct client-side logging

The SSH-orchestrated `run_experiment.sh` above works, but the reference workflow is now the same two-script model as SD: `response_tcp.sh` started on the server, `request_tcp.sh` started on the client, no SSH between them (`run_experiment.sh` is kept as a secondary, wire-level cross-check).

`examples/response-tcp-recovery.cpp` and `examples/request-tcp-recovery.cpp` were rewritten (previously a lockstep dual-service echo responder, unrelated to this test) into a matched pair:

- **Server:** publishes an 8-byte sequence counter over TCP every `--cycle` ms, incrementing by one each send. Offers the service exactly once for the whole run — unlike `notify-sample`'s periodic `stop_offer`/re-offer cycle, which would itself create gaps in the stream indistinguishable from a lost segment.
- **Client:** subscribes to that counter and times recovery the same way `request-sd.cpp` measures SD latency — directly, from its own timestamps. TCP delivers every byte reliably and in order, so a lost segment never shows up as a missing message, only as an unusually long pause before the next one arrives (the connection stalls until the retransmission lands, then vsomeip delivers the backlog). The client flags any inter-arrival gap past `--threshold × --cycle` (default 2×) as such a stall and logs it via `VSOMEIP_WARNING`:

  ```
  패킷 유실 복구 시간: 9946us (수신 간격=14946us, 정상 주기=5000us, seq 1749 -> 1750)
  ```

`response_tcp.sh` now manages loss injection itself via `tcp-recovery/inject_loss.sh` (`NO_DROP=1` to disable), so starting it is the only setup needed on the server side.

**This number is an application-level estimate, not a wire-level one** — see [tcp-recovery/README.md](tcp-recovery/README.md#결과-실측값) "방법 1" for the full breakdown. Measured on Thor (server, wired) → Orin (client, Wi-Fi), `--cycle 5`, 1-in-8 first-transmissions lost, no kernel tuning:

| | n | mean | p50 |
|---|---|---|---|
| Loss injected | 21 | 9.7 ms | 9.6 ms |
| `NO_DROP=1` baseline (natural jitter) | 2 / 594 (0.3%) | 7.2–8.5 ms range | — |

This is well above `tune_tcp_recovery.sh`'s wire-level 3.46 ms because, at this cycle, most of the gap is not network recovery at all: even with **zero loss injected**, only 594 of the ~4000 events expected at a 5 ms cycle over 20 s arrived — the server log shows `wait_until_sent: Maximum wait time for send operation exceeded`, confirming vsomeip's own TCP send-side flow control (`tcp_server_endpoint_impl::connection::wait_until_sent`) is throttling publication to what this Wi-Fi link can actually sustain, independent of the injected loss. A useful finding in its own right, and a reason to always run a `NO_DROP=1` baseline before trusting flagged events at a given `--cycle`.

### Traffic pattern matters

Recovery is only observable when a segment follows the lost one, prompting a SACK. Lockstep request/response traffic (`request-sample`/`response-sample`, the `rr` mode in `run_experiment.sh`) doesn't have that — if the response is lost, the client has nothing left to send, so recovery falls back entirely to the server's RTO. Measured: 218 ms mean, 0/292 SACK-triggered. `request-tcp-recovery`/`response-tcp-recovery` have no lockstep option at all — the server always publishes a continuous stream — so this failure mode doesn't apply to the per-device scripts above; `run_experiment.sh`'s `stream` mode uses the same style of continuous traffic (`notify-sample`/`subscribe-sample`) for the same reason, keeping `rr` mode only as a demonstration of the failure mode.

## Kernel tuning (`tcp-recovery/tune_tcp_recovery.sh`)

Run on the TCP **server** (the side that retransmits):

```diff
+ ip route replace <client-ip>/32 dev <iface> rto_min 1ms
+ sysctl -w net.ipv4.tcp_reordering=1
```

**Why:** `rto_min` (per-route) lowers the floor under a genuinely timer-driven recovery (kernel default 200 ms) — it only matters when no SACK ever arrives (lockstep traffic, or the last segment of a burst), not the common SACK path. `tcp_reordering` (global sysctl, affects every TCP connection on the host) lowers the number of duplicate-SACK signals RACK waits for before declaring a segment lost instead of merely reordered; on a link that doesn't reorder packets, that wait is pure latency with no benefit. Both settings are volatile and reversible; the script prints the revert commands.

**Measured** (Thor server → Orin client over Wi-Fi, 5 ms-cycle SOME/IP events, 1-in-8 first-transmissions lost, `[C]` = client-perceived recovery time):

| Setting | mean | p50 | n |
|---|---|---|---|
| default | 6.51 ms | 1.90 ms | 292 |
| + `rto_min 1ms` | 4.07 ms | 2.11 ms | 273 |
| + `rto_min 1ms` + `tcp_reordering=1` | **3.46 ms** | **1.64 ms** | 293 |

## What still stands between 3.46 ms and 2 ms

Breaking down the best-case 3.46 ms:

| Component | Mean contribution |
|---|---|
| Wi-Fi round trip | 1.47 ms |
| Server reacted immediately (61% of retransmits, `[B]` ≤100 µs) | ≈0 |
| Server waited 3–10 ms (RACK reorder timer, kernel-tick-quantized) | 1.61 ms |
| TLP/RTO (already suppressed by `rto_min 1ms`) | 0.28 ms |

Two things neither script touches:

1. ~~**`CONFIG_HZ=250` on both boards.**~~ **Superseded, see below** — turns out this only bites at `--cycle 5`; `--cycle 1` sidesteps it entirely without touching the kernel.
2. ~~**The Orin is on Wi-Fi.**~~ **Resolved** — see "Direct Ethernet Link" above.

## Self-stop, self-calibrating baseline, and finding `--cycle 1` (2026-09-14)

Three things, found investigating a report that loss went nearly undetected when the client was given custom `--cycle`/`--threshold` values.

**1. Reproduced the report exactly.** Server at `--cycle 1` (its real gap, per the throughput sweep above, is ~7 ms), client told `--cycle 5` (so its flagging threshold was `5 × 1000µs × 2.0 = 10ms`) — that bar sits right at or above what a real recovery actually costs, so it silently swallowed real, ongoing loss: 0 flagged out of 2743 received over 20 s.

**2. Self-calibrating baseline.** `request-tcp-recovery.cpp` no longer assumes the "normal gap" is `cycle × 1000µs` — `baseline_us()` tracks the median of the last 20 gaps that weren't themselves flagged as loss, with `--cycle` only seeding the first 5 samples. A mismatched `--cycle` is now a slower warm-up, not a correctness bug — the client converges to the link's actual behaviour on its own. Measuring this baseline directly also settled an open question: it converges to ~1.06 ms at `--cycle 1`, very close to nominal — so the traffic pattern is bursty (most gaps near the requested cycle, a minority of much larger ones pulling the *mean* throughput down to the ~15% figure in the sweep above), not uniformly slow. The client's own recovery-time number wasn't inflated by a bad baseline assumption after all; see point 3.

**3. Re-ran the wire-level cross-check at `--cycle 1` for the first time — every prior wire-level number in this document was measured at `--cycle 5`.**

| | n | mean | p50 | p90 | ≤2ms |
|---|---|---|---|---|---|
| `--cycle 1`, untuned | 92 | 0.59 ms | 0.57 ms | 0.59 ms | 100% |
| `--cycle 1`, + `tune_tcp_recovery.sh` | 71 | 0.58 ms | 0.56 ms | 0.57 ms | 100% |
| `--cycle 1`, `net.ipv4.tcp_recovery=0` (RACK off) | 112 | 1.19 ms | 0.57 ms | 1.70 ms | 90% |

**The wire-level number already clears the 2 ms target comfortably at `--cycle 1`** — the `--cycle 5` tail (§ above, tied to `CONFIG_HZ=250`-quantized RACK reorder waits) essentially disappears at this cycle. Tried disabling RACK (`tcp_recovery=0`) on the theory that its reorder-window timer was the `--cycle 5` tail's cause — the opposite happened: RACK *on* (the default) was faster. That specific hypothesis about RACK's timer was wrong; `--cycle` turned out to be what mattered, not RACK.

**What's left:** the application-level number (`request_tcp.sh`, via `--min-losses`, see below) still averages ~2.7 ms at `--cycle 1` — a real gap against the wire-level ~0.58 ms, not a baseline-calibration artifact (point 2 ruled that out). Likely vsomeip's own receive-side dispatch path (io thread → main-dispatch-thread handoff, particularly when a burst of backlogged messages lands at once after a retransmission), but not root-caused — would need instrumenting vsomeip's dispatch internals, not `request-tcp-recovery.cpp` itself. Next lead for anyone chasing the application-level number specifically below 2 ms; the wire-level ground truth is already there.

**Also added `--min-losses` (default 10) to `request-tcp-recovery.cpp`:** the client now stops itself and prints `총 수신 N건 중 M건 유실, 평균 복구 시간 Xus` once that many events are flagged, instead of only on external kill. The actual shutdown runs on a dedicated watcher thread, not from `on_message()` or the signal handler directly — `application::stop()` joins vsomeip's own io threads, and calling it from a thread vsomeip itself dispatched onto risks deadlocking that join against itself. First version of this leaked the watcher thread (never joined before `its_sample` was destroyed, `std::thread`'s destructor calls `std::terminate()` if still joinable) — reproduced live (summary printed correctly, then "terminate called without an active exception" / abort), fixed with an explicit `join()` from `main()` after `app_->start()` returns.

**Unrelated bug caught along the way:** `pgrep -x`/`pkill -x` match against the kernel's 15-character-truncated `comm` field. `response-tcp-recovery` (21 chars), `request-tcp-recovery` (21) and `subscribe-sample` (16) all exceed that, so stale-lock-file guards using `-x` against those names never actually detected a running instance — always "cleaned up" regardless, and let three `response-tcp-recovery` processes pile up fighting over port 30510 during this session's testing. Switched to `-f` (matches the full command line, no length limit) in `response_tcp.sh`, `request_tcp.sh`, and `run_experiment.sh`.

**Separately, `response_tcp.sh` now rejects `<client-ip>` equal to its own configured address** — the mistake that produced this whole investigation's original symptom (`inject_loss.sh` silently resolving to `lo` because "the peer" was actually itself), with a clear error instead of silently misrouting.

## Rejected/not applicable

- `iptables` `OUTPUT` `DROP` for loss injection — see "Why the previous method didn't work" above.
- `tc netem loss` — `sch_netem` is not built into either board's kernel (`modinfo sch_netem` fails on both), and rebuilding a kernel module is out of scope for a test harness.

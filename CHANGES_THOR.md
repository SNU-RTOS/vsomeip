# vsomeip Thor Branch Changes

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

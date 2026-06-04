# vsomeip Thor Branch Changes

## Goal

Reduce service discovery (SD) latency between the AGX Thor (server) and AGX Orin (client), specifically the duration from the client sending a **FindService** message to receiving the server's **OfferService** reply over Ethernet UDP.

---

## Device Role Assignment

| Device | Role | Binary |
|---|---|---|
| AGX Thor | **Server** — start first | `build/examples/response-sd` |
| AGX Orin | **Client** | `build/examples/request-sd` |

Starting the server before the client ensures the server is already in its main (cyclic offer) phase when the client boots, so the client's very first FindService gets an immediate unicast reply. This eliminates the `repetitions_base_delay` retry wait entirely.

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

---

## Latency Sources Eliminated

| Source | Location | Before | After |
|---|---|---|---|
| Client find debounce | `start_find_debounce_timer` | 1–2 ms random | 0 ms |
| Server offer debounce | `start_offer_debounce_timer` | 1–5 ms random | 0 ms |
| Server SD response path | `send_uni_or_multicast_offerservice` | unicast or multicast | unicast always |
| Build failure | `CMakeLists.txt` | `-Werror` on Boost ICL false positive | suppressed |

After these changes the measured SD latency (FindService sent → OfferService received) should be dominated by a single Ethernet RTT (~sub-millisecond on a direct link).

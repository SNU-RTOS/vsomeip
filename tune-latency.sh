#!/bin/bash
# Low-latency tuning for SOME/IP service discovery. Run with sudo on BOTH devices.
# All settings are volatile - they reset on reboot.
#
# NOTE: none of this compensates for running SOME/IP over Wi-Fi. A wired GbE hop
# on this LAN costs ~0.24 ms round trip; the same hop over 5 GHz Wi-Fi costs
# ~2.0 ms with multi-millisecond jitter. Use Ethernet.
set -e
[ "$(id -u)" -eq 0 ] || { echo "run me with sudo"; exit 1; }

# Auto-detect the interface holding the default route unless one is given.
IF=${1:-$(ip route show default | awk '/default/{print $5; exit}')}
echo "tuning interface: $IF"

# 1. Never enter the deep cc7 idle state (10 ms advertised exit latency).
for d in /sys/devices/system/cpu/cpu[0-9]*/cpuidle/state1/disable; do echo 1 > "$d"; done

# 2. Pin the CPUs at max clock instead of ramping up from ~970 MHz.
for g in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do echo performance > "$g"; done

# 3. Max power model + lock clocks (Jetson only).
command -v nvpmodel >/dev/null && nvpmodel -m 0 || true
command -v jetson_clocks >/dev/null && jetson_clocks || true

# 4. Wi-Fi: kill power save. The AP otherwise buffers frames until the station
#    wakes, which cost us ~3.7 ms of average RTT on the Orin.
if [ -d "/sys/class/net/$IF/wireless" ]; then
    iw dev "$IF" set power_save off
    echo "wifi power_save: $(iw dev "$IF" get power_save)"
else
    # 5. Wired: stop the PHY entering Low Power Idle between packets.
    ethtool --set-eee "$IF" eee off 2>/dev/null || echo "EEE: not supported/needed"
    ethtool -C "$IF" rx-usecs 0 rx-frames 1 2>/dev/null || echo "coalescing: driver refused"
fi

echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
echo "cc7 disabled: $(cat /sys/devices/system/cpu/cpu0/cpuidle/state1/disable)"

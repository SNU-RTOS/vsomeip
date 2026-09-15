// Copyright (C) 2014-2023 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
#include <csignal>
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <vsomeip/vsomeip.hpp>
#include <vsomeip/internal/logger.hpp>

#include "sample-ids.hpp"

// TCP packet-recovery test client. Subscribes to the sequence-number event published by
// response-tcp-recovery.cpp (the server, run via response_tcp.sh on the Thor) and times,
// purely from its own arrival timestamps, how long a segment loss + TCP retransmission
// recovery took - the same kind of measurement as request-sd.cpp's "매칭까지 처리 시간",
// just for TCP packet recovery instead of service discovery. Run via request_tcp.sh.
//
// How loss shows up at this layer: TCP delivers every byte reliably and in order, so this
// client eventually receives every sequence number regardless of loss - a lost segment
// does not show up as a missing message, it shows up as an unusually long pause before the
// next one arrives (the connection stalls until the retransmission lands, then vsomeip
// delivers the backlog in a burst). This client flags any inter-arrival gap more than
// `--threshold` times the configured `--cycle` as such a stall and logs the estimated
// recovery time for it.
//
// The "normal gap" baseline is SELF-CALIBRATING, not assumed from --cycle: vsomeip's own
// TCP publish path only delivers a roughly constant ~15% of whatever rate the server asks
// for (see tcp-recovery/README.md "발행 주기가 왜 중요한가"), so the real steady-state gap
// is consistently ~6.6-7x the server's --cycle argument, not equal to it - assuming
// otherwise (an earlier version of this tool did) silently swallowed real losses whenever
// this client's --cycle didn't happen to match what the server was actually started with.
// Instead, `baseline_us()` tracks the median of the last kBaselineWindow gaps that weren't
// themselves flagged as loss, and --cycle only seeds that estimate for the first
// kMinBaselineSamples messages before real data takes over - so a --cycle that doesn't
// match the server is no longer a correctness problem, just a slower warm-up. A
// NO_DROP=1 baseline run (see response_tcp.sh) is still worth doing to sanity-check
// --threshold, but is no longer required before trusting flagged events.
//
// This is an application-level estimate, not a wire-level measurement: besides the network
// recovery time proper, it includes vsomeip's own receive-side processing (tens to a few
// hundred µs, see the SD latency writeup) AND vsomeip's own internal TCP publish-path
// overhead mentioned above. For a wire-level cross-check unaffected by any of this, see
// tcp-recovery/analyze_recovery.py.
class client_sample {
public:
    client_sample(uint32_t _cycle, double _threshold, uint64_t _min_losses)
        : app_(vsomeip::runtime::get()->create_application()),
          cycle_(_cycle),
          threshold_(_threshold),
          min_losses_(_min_losses),
          have_last_(false),
          last_seq_(0),
          received_(0),
          flagged_(0),
          recovery_sum_us_(0),
          initialized_(false),
          stopped_(false),
          running_(true),
          watcher_thread_(std::bind(&client_sample::watch, this)) {
    }

    bool init() {
        if (!app_->init()) {
            std::cerr << "Couldn't initialize application" << std::endl;
            return false;
        }
        initialized_ = true;
        std::cout << "TCP packet-recovery client: threshold=" << threshold_
                  << "x the observed normal gap (seeded from --cycle " << cycle_
                  << "ms = " << (cycle_ * 1000) << "us until " << kMinBaselineSamples
                  << " real samples arrive, then self-calibrating)";
        if (min_losses_ > 0)
            std::cout << ", stopping after " << min_losses_ << " flagged event(s)";
        std::cout << std::endl;

        app_->register_state_handler(
                std::bind(&client_sample::on_state, this, std::placeholders::_1));
        app_->register_message_handler(
                SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, vsomeip::ANY_METHOD,
                std::bind(&client_sample::on_message, this, std::placeholders::_1));
        app_->register_availability_handler(
                SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID,
                std::bind(&client_sample::on_availability, this,
                          std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        std::set<vsomeip::eventgroup_t> its_groups;
        its_groups.insert(SAMPLE_EVENTGROUP_ID);
        app_->request_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID,
                             its_groups, vsomeip::event_type_e::ET_FIELD);
        app_->subscribe(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENTGROUP_ID);
        return true;
    }

    void start() {
        app_->start();
    }

#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
    // External stop request (Ctrl-C, or `timeout` giving up because --min-losses was never
    // reached). Safe to call from a signal handler: just wakes the watcher thread, which
    // does the actual vsomeip shutdown from its own thread context (see the class comment
    // on do_stop() for why that indirection matters).
    void stop() {
        std::lock_guard<std::mutex> its_lock(mutex_);
        running_ = false;
        condition_.notify_one();
    }
#endif

    void on_state(vsomeip::state_type_e _state) {
        if (_state == vsomeip::state_type_e::ST_REGISTERED)
            app_->request_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
    }

    void on_availability(vsomeip::service_t _service, vsomeip::instance_t _instance,
                          bool _is_available) {
        std::cout << "Service [" << std::setw(4) << std::setfill('0') << std::hex << _service
                  << "." << _instance << "] is " << (_is_available ? "available." : "NOT available.")
                  << std::dec << std::endl;
    }

    void on_message(const std::shared_ptr<vsomeip::message> &_notification) {
        auto now = std::chrono::high_resolution_clock::now();

        std::shared_ptr<vsomeip::payload> its_payload = _notification->get_payload();
        if (its_payload->get_length() != 8) {
            // e.g. the empty initial-value notification vsomeip sends right on subscribe,
            // before the service has published anything yet.
            return;
        }
        const vsomeip::byte_t *its_data = its_payload->get_data();
        uint64_t seq = 0;
        for (int i = 0; i < 8; ++i)
            seq = (seq << 8) | its_data[i];

        ++received_;

        if (have_last_) {
            auto gap_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - last_time_).count();
            int64_t base_us = baseline_us();

            if (seq != last_seq_ + 1) {
                // Should not happen over TCP - flag it, but it is not the loss signal this
                // tool measures (see the class comment).
                std::cout << "[경고] 시퀀스 불연속: " << last_seq_ << " -> " << seq << std::endl;
            }

            if (gap_us > static_cast<int64_t>(static_cast<double>(base_us) * threshold_)) {
                int64_t recovery_us = gap_us - base_us;
                if (recovery_us < 0)
                    recovery_us = 0;
                VSOMEIP_WARNING << "패킷 유실 복구 시간: " << recovery_us << "us"
                                << " (수신 간격=" << gap_us << "us, 기준 주기=" << base_us
                                << "us, seq " << last_seq_ << " -> " << seq << ")";
                std::cout << "[loss] seq " << last_seq_ << " -> " << seq << ": 복구 시간 "
                          << recovery_us << "us (수신 간격 " << gap_us << "us, 기준 " << base_us
                          << "us)" << std::endl;

                std::lock_guard<std::mutex> its_lock(mutex_);
                ++flagged_;
                recovery_sum_us_ += recovery_us;
                if (min_losses_ > 0 && flagged_ >= min_losses_)
                    condition_.notify_one();
                // Do not fold this gap into the baseline - it is the very thing we just
                // decided is NOT normal, and folding it in would drag the baseline upward.
            } else if (recent_gaps_.size() < kMinBaselineSamples || gap_us >= base_us / 4) {
                // Reject unusually SMALL gaps too, not just large ones - vsomeip delivers a
                // stall's backlog "in a burst" once the retransmission lands (see the class
                // comment), so the handful of messages right after a flagged event arrive
                // with near-zero gaps between them. Those are not the steady-state publish
                // interval either; folding them into recent_gaps_ was dragging the median
                // down (observed live: down to single-digit microseconds after one recovery
                // burst) until normal ~1ms gaps started looking anomalously large relative to
                // that collapsed baseline and got mis-flagged as new "losses" that never
                // happened on the wire - confirmed against tcp-recovery/analyze_recovery.py on
                // the same capture: 0 discrepancy between raw retransmissions and SACK-matched
                // ones, so every false positive here was purely this baseline collapsing, not
                // a real miss on the wire side. Skip this floor during initial calibration
                // (recent_gaps_ still short) since base_us is only the --cycle seed then and a
                // real first gap smaller than that seed is not an error.
                recent_gaps_.push_back(gap_us);
                if (recent_gaps_.size() > kBaselineWindow)
                    recent_gaps_.pop_front();
            }
        } else {
            have_last_ = true;
            std::cout << "첫 이벤트 수신, seq=" << seq << std::endl;
        }

        last_seq_ = seq;
        last_time_ = now;
    }

    // Runs on its own thread, never on a vsomeip-invoked one: application::stop() joins
    // vsomeip's internal io threads, and calling it from a thread vsomeip itself dispatched
    // on (e.g. straight out of on_message(), or out of a signal handler that might run on
    // any thread) risks that join deadlocking against itself. This thread just waits for
    // either "enough losses happened" or "someone asked us to stop", then performs the
    // actual shutdown safely from here.
    void watch() {
        std::unique_lock<std::mutex> its_lock(mutex_);
        condition_.wait(its_lock, [this] {
            return !running_ || (min_losses_ > 0 && flagged_ >= min_losses_);
        });
        do_stop();
    }

    void do_stop() {
        if (stopped_.exchange(true))
            return; // already stopping (e.g. both Ctrl-C and --min-losses raced)
        if (initialized_) {
            // Skipped when app_->init() itself failed (main()'s init()-failure path calls
            // stop()+join() to unblock the watcher thread cleanly): none of these were ever
            // set up in that case, and there is nothing running to stop.
            app_->clear_all_handler();
            app_->unsubscribe(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENTGROUP_ID);
            app_->release_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID);
            app_->release_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
        }
        // Print before stop(): stop() is what unblocks app_->start() on main()'s thread,
        // which then joins this thread (see join()) before letting `its_sample` in main()
        // go out of scope - so anything this thread still needs from `*this` (the counters
        // print_summary() reads) must happen before that, not after.
        print_summary();
        if (initialized_)
            app_->stop();
    }

    // Called from main(), after app_->start() returns there - never from this object's own
    // watcher thread (that would be a self-join deadlock). Blocks until do_stop() has fully
    // finished on the watcher thread, so it is safe for main() to destroy `its_sample`
    // immediately afterwards.
    void join() {
        if (watcher_thread_.joinable())
            watcher_thread_.join();
    }

    void print_summary() {
        std::cout << "총 수신 " << received_ << "건 중 " << flagged_ << "건 유실";
        if (flagged_ > 0) {
            std::cout << ", 평균 복구 시간 " << (recovery_sum_us_ / static_cast<int64_t>(flagged_))
                      << "us";
        }
        std::cout << std::endl;
    }

private:
    std::shared_ptr<vsomeip::application> app_;
    uint32_t cycle_;
    double threshold_;
    uint64_t min_losses_; // 0 = run until externally stopped (old behaviour)

    bool have_last_;
    uint64_t last_seq_;
    std::chrono::time_point<std::chrono::high_resolution_clock> last_time_;

    // Self-calibrating "normal gap" baseline - see the class comment.
    static constexpr size_t kBaselineWindow = 20;
    static constexpr size_t kMinBaselineSamples = 5;
    std::deque<int64_t> recent_gaps_; // last kBaselineWindow unflagged gaps, most recent last

    int64_t baseline_us() const {
        if (recent_gaps_.size() < kMinBaselineSamples)
            return static_cast<int64_t>(cycle_) * 1000; // not enough data yet - seed from --cycle
        std::vector<int64_t> sorted(recent_gaps_.begin(), recent_gaps_.end());
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2]; // median: robust to the occasional missed flag
    }

    uint64_t received_;
    uint64_t flagged_;
    int64_t recovery_sum_us_;

    bool initialized_; // true once app_->init() has succeeded - guards do_stop()'s app_ calls
    std::atomic<bool> stopped_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool running_;
    // running_ / mutex_ / condition_ must be initialized before this thread starts.
    std::thread watcher_thread_;
};

#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
client_sample *its_sample_ptr(nullptr);
void handle_signal(int _signal) {
    if (its_sample_ptr != nullptr && (_signal == SIGINT || _signal == SIGTERM))
        its_sample_ptr->stop();
}
#endif

int main(int argc, char **argv) {
    uint32_t cycle = 1;         // must match the server's --cycle for the threshold to mean anything
    double threshold = 3.0;     // gap > threshold * cycle is flagged as a loss+recovery stall
    uint64_t min_losses = 10;   // stop and print the summary once this many events are flagged;
                                // 0 disables this and runs until externally stopped instead

    std::string cycle_arg("--cycle");
    std::string threshold_arg("--threshold");
    std::string min_losses_arg("--min-losses");
    for (int i = 1; i < argc; i++) {
        if (cycle_arg == argv[i] && i + 1 < argc) {
            i++;
            std::stringstream converter;
            converter << argv[i];
            converter >> cycle;
        } else if (threshold_arg == argv[i] && i + 1 < argc) {
            i++;
            std::stringstream converter;
            converter << argv[i];
            converter >> threshold;
        } else if (min_losses_arg == argv[i] && i + 1 < argc) {
            i++;
            std::stringstream converter;
            converter << argv[i];
            converter >> min_losses;
        }
    }

    client_sample its_sample(cycle, threshold, min_losses);
#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
    its_sample_ptr = &its_sample;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
#endif
    if (its_sample.init()) {
        std::cout << "sample start\n";
        its_sample.start();
        its_sample.join();
        return 0;
    } else {
        // watcher_thread_ is already running (started in the constructor, before init() was
        // even tried) and blocked waiting for a stop request that will now never come from
        // do_stop()'s normal path - ask it to stop and wait for it, or its_sample's
        // destructor hits the same joinable-thread std::terminate() this whole join()
        // dance exists to avoid.
        its_sample.stop();
        its_sample.join();
        return 1;
    }
}

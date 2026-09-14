// Copyright (C) 2014-2023 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
#include <csignal>
#endif
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

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
// `--cycle` and `--threshold` must describe the ACTUAL steady-state inter-arrival gap on
// this link, not just the server's nominal --cycle: vsomeip's own TCP publish path only
// delivers a roughly constant ~15% of whatever rate the server asks for (see
// tcp-recovery/README.md "발행 주기가 왜 중요한가"), so the real gap is consistently
// ~6.6-7x the server's --cycle argument, not equal to it. If this client's --cycle doesn't
// match what the server was actually started with, the flagging threshold is checked
// against the wrong baseline - too high a threshold (client thinks the cycle is longer
// than it really is) silently swallows real losses instead of flagging them, and too low a
// threshold (client thinks it's shorter) flags normal traffic as "loss". Always start from
// a NO_DROP=1 baseline (see response_tcp.sh) at the exact --cycle you intend to use before
// trusting flagged events.
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
          stopped_(false),
          running_(true),
          watcher_thread_(std::bind(&client_sample::watch, this)) {
    }

    bool init() {
        if (!app_->init()) {
            std::cerr << "Couldn't initialize application" << std::endl;
            return false;
        }
        std::cout << "TCP packet-recovery client: cycle=" << cycle_ << "ms, threshold="
                  << threshold_ << "x (recovery flagged when a gap exceeds "
                  << static_cast<uint32_t>(cycle_ * threshold_) << "ms)";
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
            auto expected_us = static_cast<int64_t>(cycle_) * 1000;

            if (seq != last_seq_ + 1) {
                // Should not happen over TCP - flag it, but it is not the loss signal this
                // tool measures (see the class comment).
                std::cout << "[경고] 시퀀스 불연속: " << last_seq_ << " -> " << seq << std::endl;
            }

            if (gap_us > static_cast<int64_t>(static_cast<double>(expected_us) * threshold_)) {
                int64_t recovery_us = gap_us - expected_us;
                if (recovery_us < 0)
                    recovery_us = 0;
                VSOMEIP_WARNING << "패킷 유실 복구 시간: " << recovery_us << "us"
                                << " (수신 간격=" << gap_us << "us, 정상 주기=" << expected_us
                                << "us, seq " << last_seq_ << " -> " << seq << ")";
                std::cout << "[loss] seq " << last_seq_ << " -> " << seq << ": 복구 시간 "
                          << recovery_us << "us (수신 간격 " << gap_us << "us)" << std::endl;

                std::lock_guard<std::mutex> its_lock(mutex_);
                ++flagged_;
                recovery_sum_us_ += recovery_us;
                if (min_losses_ > 0 && flagged_ >= min_losses_)
                    condition_.notify_one();
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
        app_->clear_all_handler();
        app_->unsubscribe(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENTGROUP_ID);
        app_->release_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID);
        app_->release_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
        app_->stop();
        print_summary();
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

    uint64_t received_;
    uint64_t flagged_;
    int64_t recovery_sum_us_;

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
        return 0;
    } else {
        return 1;
    }
}

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
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <vsomeip/vsomeip.hpp>
#include <vsomeip/internal/logger.hpp>

#include "sample-ids.hpp"

// "Hybrid" recovery test client - pairs with response-hybrid-recovery.cpp. Subscribes to
// the same TCP event stream as request-tcp-recovery.cpp (same self-calibrating baseline,
// same circuit breaker - see that file's class comment for the full history of why both
// exist), but detects a stall a different way and does something about it once detected.
//
// request-tcp-recovery.cpp's detection is retrospective: it only notices a gap was too
// long once the NEXT event actually arrives and it can compute how long the pause was -
// by which point TCP has already delivered the data, so there is nothing left to
// "recover" faster. That is fine for *measuring* the native TCP number, but useless as the
// trigger for a parallel fast-recovery attempt, which has to fire *while* the stream is
// still stalled to mean anything.
//
// This client instead runs a proactive watchdog thread that polls every kWatchdogPollUs:
// if more than threshold * baseline_us() has elapsed since the last event with no new one
// arriving, it declares a stall *right then* (matching the definition proposed for this
// test: client expects data, a timeout passes with nothing arriving, that is the moment
// loss is identified) and immediately fires a UDP RPC at response-hybrid-recovery.cpp's
// second instance - the same fast side-channel as request-udp-recovery.cpp's, timed the
// same way. Two numbers come out of this, per stall:
//   [hybrid]    stall declared -> UDP RPC response received (the number this test exists
//               to measure - is grafting a fast side-channel onto an otherwise-untouched
//               TCP stream enough to get fresh data in ~300-600us, matching the pure-UDP
//               result, instead of waiting ~2.6-2.7ms for TCP's own retransmission?)
//   [tcp-native] stall declared -> the TCP stream's own delayed event actually arrives
//               (reported for comparison only - the UDP answer normally arrives first;
//               this is "how long it would have taken without the side-channel")
class client_sample {
public:
    client_sample(uint32_t _cycle, double _threshold, uint64_t _min_losses)
        : app_(vsomeip::runtime::get()->create_application()),
          recovery_request_(vsomeip::runtime::get()->create_request(false)),
          cycle_(_cycle),
          threshold_(_threshold),
          min_losses_(_min_losses),
          have_last_(false),
          last_seq_(0),
          stall_declared_(false),
          received_(0),
          flagged_(0),
          hybrid_recovery_sum_us_(0),
          tcp_flagged_(0),
          tcp_recovery_sum_us_(0),
          initialized_(false),
          stopped_(false),
          running_(true),
          event_available_(false),
          rpc_available_(false),
          was_fully_available_(false),
          service_lost_(false),
          watcher_thread_(std::bind(&client_sample::watch, this)),
          watchdog_thread_(std::bind(&client_sample::watchdog_loop, this)) {
    }

    bool init() {
        if (!app_->init()) {
            std::cerr << "Couldn't initialize application" << std::endl;
            return false;
        }
        initialized_ = true;
        std::cout << "Hybrid recovery client: TCP event stream + proactive watchdog"
                     " (threshold=" << threshold_ << "x self-calibrated baseline) -> UDP"
                     " recovery RPC on stall";
        if (min_losses_ > 0)
            std::cout << ", stopping after " << min_losses_ << " flagged event(s)";
        std::cout << ", stopping when the server ends the session" << std::endl;

        app_->register_state_handler(
                std::bind(&client_sample::on_state, this, std::placeholders::_1));
        app_->register_message_handler(
                SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, vsomeip::ANY_METHOD,
                std::bind(&client_sample::on_message, this, std::placeholders::_1));
        app_->register_message_handler(
                SAMPLE_SERVICE_ID, RECOVERY_RPC_INSTANCE_ID, SAMPLE_METHOD_ID,
                std::bind(&client_sample::on_recovery_response, this, std::placeholders::_1));
        app_->register_availability_handler(
                SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID,
                std::bind(&client_sample::on_event_availability, this,
                          std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));
        app_->register_availability_handler(
                SAMPLE_SERVICE_ID, RECOVERY_RPC_INSTANCE_ID,
                std::bind(&client_sample::on_rpc_availability, this,
                          std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        std::set<vsomeip::eventgroup_t> its_groups;
        its_groups.insert(SAMPLE_EVENTGROUP_ID);
        app_->request_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID,
                             its_groups, vsomeip::event_type_e::ET_FIELD);
        app_->subscribe(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENTGROUP_ID);

        recovery_request_->set_service(SAMPLE_SERVICE_ID);
        recovery_request_->set_instance(RECOVERY_RPC_INSTANCE_ID);
        recovery_request_->set_method(SAMPLE_METHOD_ID);
        return true;
    }

    void start() {
        app_->start();
    }

#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
    void stop() {
        std::lock_guard<std::mutex> its_lock(mutex_);
        running_ = false;
        condition_.notify_one();
    }
#endif

    void on_state(vsomeip::state_type_e _state) {
        if (_state == vsomeip::state_type_e::ST_REGISTERED) {
            app_->request_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
            app_->request_service(SAMPLE_SERVICE_ID, RECOVERY_RPC_INSTANCE_ID);
        }
    }

    void on_event_availability(vsomeip::service_t, vsomeip::instance_t, bool _is_available) {
        std::cout << "Event service is " << (_is_available ? "available." : "NOT available.")
                  << std::endl;
        event_available_ = _is_available;
        check_full_availability();
    }

    void on_rpc_availability(vsomeip::service_t, vsomeip::instance_t, bool _is_available) {
        std::cout << "Recovery RPC service is " << (_is_available ? "available." : "NOT available.")
                  << std::endl;
        rpc_available_ = _is_available;
        check_full_availability();
    }

    // Same rationale as request-tcp-recovery.cpp's/request-udp-recovery.cpp's
    // on_availability(): follow the server once it ends the SD session on its own.
    void check_full_availability() {
        bool now_full = event_available_ && rpc_available_;
        if (now_full) {
            was_fully_available_ = true;
        } else if (was_fully_available_) {
            std::lock_guard<std::mutex> its_lock(mutex_);
            service_lost_ = true;
            condition_.notify_one();
        }
    }

    void on_message(const std::shared_ptr<vsomeip::message> &_notification) {
        auto now = std::chrono::high_resolution_clock::now();

        std::shared_ptr<vsomeip::payload> its_payload = _notification->get_payload();
        if (its_payload->get_length() != 8)
            return; // initial empty field notification

        const vsomeip::byte_t *its_data = its_payload->get_data();
        uint64_t seq = 0;
        for (int i = 0; i < 8; ++i)
            seq = (seq << 8) | its_data[i];

        ++received_;

        if (have_last_) {
            int64_t gap_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - last_event_time_).count();

            bool was_stalled;
            std::chrono::high_resolution_clock::time_point stall_at;
            {
                std::lock_guard<std::mutex> its_lock(watchdog_mutex_);
                was_stalled = stall_declared_;
                stall_at = stall_declared_at_;
                stall_declared_ = false; // this stall's cycle is fully closed either way -
                                          // the watchdog is free to declare a fresh one
                last_event_time_ = now;
            }

            if (was_stalled) {
                // The watchdog already fired the UDP side-channel for this stall (see
                // watchdog_loop()/on_recovery_response()) - this is TCP's own, separately
                // timed, eventual delivery of the same data, kept only for comparison.
                int64_t tcp_native_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        now - stall_at).count();
                ++tcp_flagged_;
                tcp_recovery_sum_us_ += tcp_native_us;
                std::cout << "[tcp-native] seq " << last_seq_ << " -> " << seq
                          << ": 네이티브 TCP 복구 시간 " << tcp_native_us << "us (참고용)"
                          << std::endl;
                if (++consecutive_flags_ > kMaxConsecutiveFlags) {
                    std::cout << "[기준 재설정] 연속 " << consecutive_flags_
                              << "건 유실 flag - 기준선이 무너진 것으로 보고 재보정합니다"
                              << std::endl;
                    recent_gaps_.clear();
                    consecutive_flags_ = 0;
                }
            } else {
                consecutive_flags_ = 0;
                int64_t base_us = baseline_us();
                // Backstop only - the watchdog is expected to have already caught any real
                // stall proactively (that is the whole point of this client); a gap this
                // large slipping through anyway (e.g. watchdog thread starved) is not folded
                // into the baseline, same reasoning as request-tcp-recovery.cpp's own flag
                // branch, but is not separately counted here either since it was never
                // declared as a stall in the first place.
                if (gap_us <= static_cast<int64_t>(static_cast<double>(base_us) * threshold_)) {
                    // Same small-gap floor as request-tcp-recovery.cpp: reject the
                    // near-zero gaps from a post-stall delivery burst too, not just large
                    // ones - see that file's class/on_message comments for the full
                    // baseline-collapse history this guards against.
                    if (recent_gaps_.size() < kMinBaselineSamples || gap_us >= base_us / 4) {
                        recent_gaps_.push_back(gap_us);
                        if (recent_gaps_.size() > kBaselineWindow)
                            recent_gaps_.pop_front();
                    }
                }
            }
        } else {
            have_last_ = true;
            std::lock_guard<std::mutex> its_lock(watchdog_mutex_);
            last_event_time_ = now;
            std::cout << "첫 이벤트 수신, seq=" << seq << std::endl;
        }

        last_seq_ = seq;
    }

    // Runs continuously on its own thread from construction. Polling, not an
    // event-driven timer, deliberately: this needs to notice "nothing has arrived for a
    // while" from the *absence* of an event, which an asio deadline timer could also do,
    // but a plain poll loop is far simpler to get right here and kWatchdogPollUs (50us) is
    // small next to the multi-hundred-microsecond-to-millisecond timescale being measured.
    void watchdog_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::microseconds(kWatchdogPollUs));
            if (!have_last_)
                continue;

            auto now = std::chrono::high_resolution_clock::now();
            int64_t timeout_us;
            bool should_fire = false;
            {
                std::lock_guard<std::mutex> its_lock(watchdog_mutex_);
                if (stall_declared_)
                    continue; // already fired for the current gap, waiting on its resolution
                int64_t since_last_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        now - last_event_time_).count();
                timeout_us = static_cast<int64_t>(static_cast<double>(baseline_us()) * threshold_);
                if (since_last_us > timeout_us) {
                    stall_declared_ = true;
                    stall_declared_at_ = now;
                    should_fire = true;
                }
            }
            if (should_fire) {
                {
                    std::lock_guard<std::mutex> its_lock(mutex_);
                    ++flagged_;
                    if (min_losses_ > 0 && flagged_ >= min_losses_)
                        condition_.notify_one();
                }
                std::cout << "[stall] " << timeout_us << "us 동안 새 이벤트 없음 - "
                             "복구 RPC 요청" << std::endl;
                app_->send(recovery_request_);
            }
        }
    }

    void on_recovery_response(const std::shared_ptr<vsomeip::message> &_response) {
        if (_response->get_message_type() != vsomeip::message_type_e::MT_RESPONSE)
            return;
        auto now = std::chrono::high_resolution_clock::now();

        std::chrono::high_resolution_clock::time_point stall_at;
        {
            std::lock_guard<std::mutex> its_lock(watchdog_mutex_);
            if (!stall_declared_)
                return; // stray/duplicate, or the native TCP delivery already closed this
                        // stall's cycle before the UDP answer got here (rare, means TCP was
                        // actually faster this one time) - nothing to time
            stall_at = stall_declared_at_;
        }
        int64_t hybrid_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now - stall_at).count();

        VSOMEIP_WARNING << "하이브리드 복구 시간: " << hybrid_us << "us";
        std::cout << "[hybrid] 복구 시간 " << hybrid_us << "us" << std::endl;

        std::lock_guard<std::mutex> its_lock(mutex_);
        hybrid_recovery_sum_us_ += hybrid_us;
        ++hybrid_count_;
    }

    void watch() {
        std::unique_lock<std::mutex> its_lock(mutex_);
        condition_.wait(its_lock, [this] {
            return !running_ || service_lost_ || (min_losses_ > 0 && flagged_ >= min_losses_);
        });
        do_stop();
    }

    void do_stop() {
        if (stopped_.exchange(true))
            return;
        running_ = false; // let watchdog_loop() exit its poll loop too
        if (initialized_) {
            app_->clear_all_handler();
            app_->unsubscribe(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENTGROUP_ID);
            app_->release_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID);
            app_->release_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
            app_->release_service(SAMPLE_SERVICE_ID, RECOVERY_RPC_INSTANCE_ID);
        }
        print_summary();
        if (initialized_)
            app_->stop();
    }

    void join() {
        if (watcher_thread_.joinable())
            watcher_thread_.join();
        if (watchdog_thread_.joinable())
            watchdog_thread_.join();
    }

    void print_summary() {
        std::cout << "총 수신 " << received_ << "건 중 " << flagged_ << "건 유실 식별(watchdog)";
        if (hybrid_count_ > 0) {
            std::cout << ", [hybrid] 평균 복구 시간 "
                      << (hybrid_recovery_sum_us_ / static_cast<int64_t>(hybrid_count_))
                      << "us (n=" << hybrid_count_ << ")";
        }
        if (tcp_flagged_ > 0) {
            std::cout << ", [tcp-native] 평균 복구 시간 "
                      << (tcp_recovery_sum_us_ / static_cast<int64_t>(tcp_flagged_))
                      << "us (n=" << tcp_flagged_ << ", 참고용)";
        }
        if (service_lost_)
            std::cout << " (서버가 세션을 종료함)";
        std::cout << std::endl;
    }

private:
    std::shared_ptr<vsomeip::application> app_;
    std::shared_ptr<vsomeip::message> recovery_request_;
    uint32_t cycle_;
    double threshold_;
    uint64_t min_losses_;

    bool have_last_;
    uint64_t last_seq_;
    // Guarded by watchdog_mutex_, not mutex_ - read/written from on_message() (a
    // vsomeip-dispatched thread), the watchdog thread, and on_recovery_response() (also
    // vsomeip-dispatched, possibly a different thread than on_message()'s).
    std::chrono::time_point<std::chrono::high_resolution_clock> last_event_time_;
    bool stall_declared_;
    std::chrono::time_point<std::chrono::high_resolution_clock> stall_declared_at_;
    std::mutex watchdog_mutex_;

    static constexpr int64_t kWatchdogPollUs = 50;

    static constexpr size_t kBaselineWindow = 20;
    static constexpr size_t kMinBaselineSamples = 5;
    static constexpr int kMaxConsecutiveFlags = 15;
    std::deque<int64_t> recent_gaps_;
    int consecutive_flags_ = 0;

    int64_t baseline_us() const {
        if (recent_gaps_.size() < kMinBaselineSamples)
            return static_cast<int64_t>(cycle_) * 1000;
        std::vector<int64_t> sorted(recent_gaps_.begin(), recent_gaps_.end());
        std::sort(sorted.begin(), sorted.end());
        return sorted[sorted.size() / 2];
    }

    uint64_t received_;
    uint64_t flagged_;             // stalls the watchdog declared
    int64_t hybrid_recovery_sum_us_;
    uint64_t hybrid_count_ = 0;    // UDP responses actually received and timed
    uint64_t tcp_flagged_;         // stalls for which TCP's own delivery was also observed
    int64_t tcp_recovery_sum_us_;

    bool initialized_;
    std::atomic<bool> stopped_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool running_;
    bool event_available_;
    bool rpc_available_;
    bool was_fully_available_;
    bool service_lost_;
    std::thread watcher_thread_;
    std::thread watchdog_thread_;
};

#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
client_sample *its_sample_ptr(nullptr);
void handle_signal(int _signal) {
    if (its_sample_ptr != nullptr && (_signal == SIGINT || _signal == SIGTERM))
        its_sample_ptr->stop();
}
#endif

int main(int argc, char **argv) {
    uint32_t cycle = 1;
    double threshold = 3.0;
    uint64_t min_losses = 0;

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
        its_sample.stop();
        its_sample.join();
        return 1;
    }
}

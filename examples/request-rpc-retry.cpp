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
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <vsomeip/vsomeip.hpp>
#include <vsomeip/internal/logger.hpp>

#include "sample-ids.hpp"

// RPC timeout+retry test client - pairs with response-rpc-retry.cpp. Sends one request at
// a time (--cycle ms apart) over UDP and waits up to --timeout-us for the response. If
// nothing comes back in time, it is the client itself - not a kernel retransmission timer,
// not a stream-level statistical judgement - that declares the request lost and resends it,
// exactly the scenario described for this test: "client requests, a timeout passes with
// nothing back, client retransmits - measure from the retransmission to the answer finally
// arriving." Because each request is independent (no cyclic baseline to confuse it with),
// --timeout-us can be set close to the link's actual RTT (measured standalone, with
// npdu-default-timings fixed: ~300-1200us on this link - see CHANGES_THOR.md) instead of
// the multi-millisecond margin request-tcp-recovery.cpp/request-hybrid-recovery.cpp need
// to avoid mistaking normal cyclic jitter for a real loss.
class client_sample {
public:
    client_sample(uint32_t _cycle_ms, int64_t _timeout_us, int _max_retries,
                  uint64_t _min_recoveries)
        : app_(vsomeip::runtime::get()->create_application()),
          request_(vsomeip::runtime::get()->create_request(false)),
          cycle_ms_(_cycle_ms),
          timeout_us_(_timeout_us),
          max_retries_(_max_retries),
          min_recoveries_(_min_recoveries),
          pending_(false),
          retry_count_(0),
          sent_(0),
          received_(0),
          normal_count_(0),
          normal_sum_us_(0),
          recovered_(0),
          recovery_sum_us_(0),
          gave_up_(0),
          initialized_(false),
          stopped_(false),
          running_(true),
          available_(false),
          was_available_(false),
          service_lost_(false),
          watcher_thread_(std::bind(&client_sample::watch, this)),
          ping_thread_(std::bind(&client_sample::ping_loop, this)),
          watchdog_thread_(std::bind(&client_sample::watchdog_loop, this)) {
    }

    bool init() {
        if (!app_->init()) {
            std::cerr << "Couldn't initialize application" << std::endl;
            return false;
        }
        initialized_ = true;
        std::cout << "RPC timeout+retry client: timeout=" << timeout_us_
                  << "us, max_retries=" << max_retries_;
        if (min_recoveries_ > 0)
            std::cout << ", stopping after " << min_recoveries_ << " recoveries";
        std::cout << std::endl;

        app_->register_state_handler(
                std::bind(&client_sample::on_state, this, std::placeholders::_1));
        app_->register_message_handler(
                SAMPLE_SERVICE_ID, RPC_RETRY_INSTANCE_ID, SAMPLE_METHOD_ID,
                std::bind(&client_sample::on_response, this, std::placeholders::_1));
        app_->register_availability_handler(
                SAMPLE_SERVICE_ID, RPC_RETRY_INSTANCE_ID,
                std::bind(&client_sample::on_availability, this,
                          std::placeholders::_1, std::placeholders::_2, std::placeholders::_3));

        request_->set_service(SAMPLE_SERVICE_ID);
        request_->set_instance(RPC_RETRY_INSTANCE_ID);
        request_->set_method(SAMPLE_METHOD_ID);
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
        if (_state == vsomeip::state_type_e::ST_REGISTERED)
            app_->request_service(SAMPLE_SERVICE_ID, RPC_RETRY_INSTANCE_ID);
    }

    void on_availability(vsomeip::service_t, vsomeip::instance_t, bool _is_available) {
        std::cout << "Service is " << (_is_available ? "available." : "NOT available.")
                  << std::endl;
        available_ = _is_available;
        if (_is_available) {
            was_available_ = true;
        } else if (was_available_) {
            std::lock_guard<std::mutex> its_lock(mutex_);
            service_lost_ = true;
            condition_.notify_one();
        }
    }

    // Fires a fresh request every cycle_ms_ - but only if the previous one has already
    // been resolved (answered, or given up on after max_retries_); otherwise skips this
    // tick rather than piling up a second request on top of one still in flight.
    void ping_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cycle_ms_));
            if (!running_ || !available_)
                continue;
            std::lock_guard<std::mutex> its_lock(req_mutex_);
            if (pending_)
                continue;
            send_locked(false);
        }
    }

    // Caller must hold req_mutex_.
    void send_locked(bool _is_retry) {
        pending_ = true;
        retry_count_ = _is_retry ? retry_count_ + 1 : 0;
        last_sent_at_ = std::chrono::high_resolution_clock::now();
        ++sent_;
        app_->send(request_);
    }

    // Polls for "it has been too long since the last send and still nothing back" -
    // see request-hybrid-recovery.cpp's watchdog_loop() for why polling instead of an
    // event-driven timer (same reasoning, same poll granularity).
    void watchdog_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::microseconds(kWatchdogPollUs));
            auto now = std::chrono::high_resolution_clock::now();
            bool retry_now = false;
            bool give_up_now = false;
            {
                std::lock_guard<std::mutex> its_lock(req_mutex_);
                if (!pending_)
                    continue;
                int64_t since_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        now - last_sent_at_).count();
                if (since_us > timeout_us_) {
                    if (retry_count_ < max_retries_) {
                        std::cout << "[timeout] " << timeout_us_ << "us 동안 응답 없음 - "
                                  << (retry_count_ + 1) << "번째 재전송" << std::endl;
                        send_locked(true);
                        retry_now = true;
                    } else {
                        pending_ = false;
                        give_up_now = true;
                    }
                }
            }
            if (give_up_now) {
                std::lock_guard<std::mutex> its_lock(mutex_);
                ++gave_up_;
                std::cout << "[give-up] " << max_retries_ << "번 재전송해도 응답 없음 - 포기"
                          << std::endl;
            }
            (void)retry_now;
        }
    }

    void on_response(const std::shared_ptr<vsomeip::message> &_response) {
        if (_response->get_message_type() != vsomeip::message_type_e::MT_RESPONSE)
            return;
        auto now = std::chrono::high_resolution_clock::now();

        bool was_pending, was_retry;
        std::chrono::high_resolution_clock::time_point sent_at;
        {
            std::lock_guard<std::mutex> its_lock(req_mutex_);
            was_pending = pending_;
            if (!was_pending)
                return; // stray/duplicate, or arrived after we'd already given up
            was_retry = retry_count_ > 0;
            sent_at = last_sent_at_;
            pending_ = false;
        }
        int64_t rtt_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now - sent_at).count();

        std::lock_guard<std::mutex> its_lock(mutex_);
        ++received_;
        if (was_retry) {
            VSOMEIP_WARNING << "재전송 후 복구 시간: " << rtt_us << "us";
            std::cout << "[recovered] 재전송 후 복구 시간 " << rtt_us << "us" << std::endl;
            ++recovered_;
            recovery_sum_us_ += rtt_us;
            if (min_recoveries_ > 0 && recovered_ >= min_recoveries_)
                condition_.notify_one();
        } else {
            ++normal_count_;
            normal_sum_us_ += rtt_us;
        }
    }

    void watch() {
        std::unique_lock<std::mutex> its_lock(mutex_);
        condition_.wait(its_lock, [this] {
            return !running_ || service_lost_ ||
                   (min_recoveries_ > 0 && recovered_ >= min_recoveries_);
        });
        do_stop();
    }

    void do_stop() {
        if (stopped_.exchange(true))
            return;
        running_ = false;
        if (initialized_) {
            app_->clear_all_handler();
            app_->release_service(SAMPLE_SERVICE_ID, RPC_RETRY_INSTANCE_ID);
        }
        print_summary();
        if (initialized_)
            app_->stop();
    }

    void join() {
        if (watcher_thread_.joinable())
            watcher_thread_.join();
        if (ping_thread_.joinable())
            ping_thread_.join();
        if (watchdog_thread_.joinable())
            watchdog_thread_.join();
    }

    void print_summary() {
        std::cout << "전송 " << sent_ << "건(재전송 포함), 응답 " << received_ << "건, "
                  << "포기 " << gave_up_ << "건";
        if (normal_count_ > 0) {
            std::cout << ", 정상 왕복 평균 " << (normal_sum_us_ / static_cast<int64_t>(normal_count_))
                      << "us (n=" << normal_count_ << ")";
        }
        if (recovered_ > 0) {
            std::cout << ", 재전송 후 복구 평균 "
                      << (recovery_sum_us_ / static_cast<int64_t>(recovered_))
                      << "us (n=" << recovered_ << ")";
        }
        if (service_lost_)
            std::cout << " (서버가 세션을 종료함)";
        std::cout << std::endl;
    }

private:
    std::shared_ptr<vsomeip::application> app_;
    std::shared_ptr<vsomeip::message> request_;
    uint32_t cycle_ms_;
    int64_t timeout_us_;
    int max_retries_;
    uint64_t min_recoveries_;

    static constexpr int64_t kWatchdogPollUs = 50;

    // Guarded by req_mutex_ - read/written from ping_loop(), watchdog_loop(), and
    // on_response() (vsomeip-dispatched, possibly yet another thread).
    std::mutex req_mutex_;
    bool pending_;
    int retry_count_;
    std::chrono::time_point<std::chrono::high_resolution_clock> last_sent_at_;

    uint64_t sent_;
    uint64_t received_;
    uint64_t normal_count_;
    int64_t normal_sum_us_;
    uint64_t recovered_;
    int64_t recovery_sum_us_;
    uint64_t gave_up_;

    bool initialized_;
    std::atomic<bool> stopped_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool running_;
    bool available_;
    bool was_available_;
    bool service_lost_;
    std::thread watcher_thread_;
    std::thread ping_thread_;
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
    uint32_t cycle_ms = 5;
    int64_t timeout_us = 2000;
    int max_retries = 5;
    uint64_t min_recoveries = 10;

    std::string cycle_arg("--cycle");
    std::string timeout_arg("--timeout-us");
    std::string retries_arg("--max-retries");
    std::string min_arg("--min-recoveries");
    for (int i = 1; i < argc; i++) {
        if (cycle_arg == argv[i] && i + 1 < argc) {
            i++; std::stringstream c; c << argv[i]; c >> cycle_ms;
        } else if (timeout_arg == argv[i] && i + 1 < argc) {
            i++; std::stringstream c; c << argv[i]; c >> timeout_us;
        } else if (retries_arg == argv[i] && i + 1 < argc) {
            i++; std::stringstream c; c << argv[i]; c >> max_retries;
        } else if (min_arg == argv[i] && i + 1 < argc) {
            i++; std::stringstream c; c << argv[i]; c >> min_recoveries;
        }
    }

    client_sample its_sample(cycle_ms, timeout_us, max_retries, min_recoveries);
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

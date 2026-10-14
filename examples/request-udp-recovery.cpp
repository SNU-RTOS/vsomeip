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

// Prototype "fast recovery" client - pairs with response-udp-recovery.cpp. Subscribes to
// the same incrementing-counter event as request-tcp-recovery.cpp, but over UDP, and
// measures recovery a completely different way: UDP has no built-in reliability, so a
// lost event is not a stall-then-burst (as it is on TCP) - it is a direct, deterministic
// gap in the sequence number this client already receives every message. No heuristic,
// no self-calibrating baseline, no false positives to chase (unlike
// request-tcp-recovery.cpp's long history of baseline-collapse bugs) - a gap simply IS a
// loss, known the instant the next event arrives.
//
// On a gap, this client immediately fires an RPC request at response-udp-recovery.cpp's
// second instance (RECOVERY_RPC_INSTANCE_ID - a separate UDP port loss injection never
// touches) asking for fresh data, and times the round trip. That round-trip time is the
// number being compared against request-tcp-recovery.cpp's own application-level
// "패킷 유실 복구 시간" - both measure "from noticing a gap to having fresh data in hand",
// just via two different recovery mechanisms (kernel TCP retransmission vs. a
// hand-rolled UDP RPC), to test whether the ~2ms-plus cost found on the TCP side is
// TCP-stack-specific or a generic socket-receive-path cost on this host.
class client_sample {
public:
    client_sample(uint32_t _cycle, uint64_t _min_losses)
        : app_(vsomeip::runtime::get()->create_application()),
          cycle_(_cycle),
          min_losses_(_min_losses),
          recovery_request_(vsomeip::runtime::get()->create_request(false)),
          have_last_(false),
          last_seq_(0),
          received_(0),
          flagged_(0),
          recovery_sum_us_(0),
          request_pending_(false),
          event_available_(false),
          rpc_available_(false),
          was_fully_available_(false),
          service_lost_(false),
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
        std::cout << "UDP fast-recovery client: deterministic seq-gap detection"
                     " (no heuristic, no baseline)";
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

    // Same rationale as request-tcp-recovery.cpp's on_availability(): once the server has
    // offered both instances at least once and then stops (its own --min-losses reached,
    // or just shut down), follow it instead of hanging forever waiting for a local count
    // that may never come.
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
            if (seq != last_seq_ + 1) {
                // Deterministic: UDP does not reorder-and-deliver like TCP does, so a
                // non-contiguous seq IS a loss, not a maybe. (seq - last_seq_ - 1) events
                // were lost, but - matching request-tcp-recovery.cpp's convention of one
                // flagged "event" per stall regardless of how many segments it covered -
                // this fires one recovery request per gap, not one per missing seq.
                std::cout << "[gap] seq " << last_seq_ << " -> " << seq << " ("
                          << (seq - last_seq_ - 1) << " missing) - requesting recovery"
                          << std::endl;
                trigger_recovery(now);
            }
        } else {
            have_last_ = true;
            std::cout << "첫 이벤트 수신, seq=" << seq << std::endl;
        }
        last_seq_ = seq;
    }

    void trigger_recovery(std::chrono::high_resolution_clock::time_point _now) {
        {
            std::lock_guard<std::mutex> its_lock(mutex_);
            if (request_pending_)
                return; // one in flight already - do not pile up overlapping requests
            request_pending_ = true;
            request_sent_at_ = _now;
        }
        app_->send(recovery_request_);
    }

    void on_recovery_response(const std::shared_ptr<vsomeip::message> &_response) {
        if (_response->get_message_type() != vsomeip::message_type_e::MT_RESPONSE)
            return;
        auto now = std::chrono::high_resolution_clock::now();

        int64_t recovery_us;
        {
            std::lock_guard<std::mutex> its_lock(mutex_);
            if (!request_pending_)
                return; // stray/duplicate - nothing we're timing right now
            request_pending_ = false;
            recovery_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    now - request_sent_at_).count();
        }

        VSOMEIP_WARNING << "UDP 유실 복구 시간: " << recovery_us << "us";
        std::cout << "[recovered] 복구 시간 " << recovery_us << "us" << std::endl;

        std::lock_guard<std::mutex> its_lock(mutex_);
        ++flagged_;
        recovery_sum_us_ += recovery_us;
        if (min_losses_ > 0 && flagged_ >= min_losses_)
            condition_.notify_one();
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
    }

    void print_summary() {
        std::cout << "총 수신 " << received_ << "건 중 " << flagged_ << "건 유실";
        if (flagged_ > 0) {
            std::cout << ", 평균 복구 시간 " << (recovery_sum_us_ / static_cast<int64_t>(flagged_))
                      << "us";
        }
        if (service_lost_) {
            std::cout << " (서버가 세션을 종료함)";
        }
        std::cout << std::endl;
    }

private:
    std::shared_ptr<vsomeip::application> app_;
    uint32_t cycle_;
    uint64_t min_losses_;
    std::shared_ptr<vsomeip::message> recovery_request_;

    bool have_last_;
    uint64_t last_seq_;

    uint64_t received_;
    uint64_t flagged_;
    int64_t recovery_sum_us_;

    bool request_pending_;
    std::chrono::time_point<std::chrono::high_resolution_clock> request_sent_at_;

    bool event_available_;
    bool rpc_available_;
    bool was_fully_available_;
    bool service_lost_;

    bool initialized_;
    std::atomic<bool> stopped_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool running_;
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
    uint32_t cycle = 1;
    uint64_t min_losses = 0;

    std::string cycle_arg("--cycle");
    std::string min_losses_arg("--min-losses");
    for (int i = 1; i < argc; i++) {
        if (cycle_arg == argv[i] && i + 1 < argc) {
            i++;
            std::stringstream converter;
            converter << argv[i];
            converter >> cycle;
        } else if (min_losses_arg == argv[i] && i + 1 < argc) {
            i++;
            std::stringstream converter;
            converter << argv[i];
            converter >> min_losses;
        }
    }

    client_sample its_sample(cycle, min_losses);
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

// Copyright (C) 2014-2023 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
#include <csignal>
#endif
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#include <vsomeip/vsomeip.hpp>

#include "sample-ids.hpp"

// "Hybrid" recovery test service - the main data path is exactly response-tcp-recovery.cpp
// (TCP event stream, kernel-level retransmission on loss), PLUS a second UDP-only instance
// (RECOVERY_RPC_INSTANCE_ID, same role as response-udp-recovery.cpp's) that answers a
// "give me fresh data now" RPC immediately. The pairing client (request-hybrid-recovery.cpp)
// keeps using the TCP stream as its primary source, but - the actual point of this test -
// does not just passively wait for TCP's own retransmission once it notices the stream has
// stalled: it fires this RPC right away and measures that round trip as the *effective*
// recovery time, while separately also recording how long the native TCP stream itself took
// to catch up on its own, for direct side-by-side comparison in the same run.
//
// See CHANGES_THOR.md for why this is worth testing: request-tcp-recovery.cpp's
// application-level number is stuck around 2.6-2.7ms (receive-side kernel cost, not fixable
// without a kernel rebuild - see the bpftrace section), while a `npdu-default-timings`-fixed
// UDP RPC like this one's measures 300-600us for the exact same kind of round trip. This
// tests whether grafting that fast RPC onto an otherwise-unmodified TCP stream, instead of
// replacing the stream's transport entirely, still gets the client fresh data in ~300-600us.
class service_sample {
public:
    service_sample(uint32_t _cycle)
        : app_(vsomeip::runtime::get()->create_application()),
          cycle_(_cycle),
          blocked_(false),
          running_(true),
          sequence_(0),
          offer_thread_(std::bind(&service_sample::run, this)) {
    }

    bool init() {
        std::lock_guard<std::mutex> its_lock(mutex_);

        if (!app_->init()) {
            std::cerr << "Couldn't initialize application" << std::endl;
            return false;
        }
        app_->register_state_handler(
                std::bind(&service_sample::on_state, this, std::placeholders::_1));
        app_->register_message_handler(
                SAMPLE_SERVICE_ID, RECOVERY_RPC_INSTANCE_ID, SAMPLE_METHOD_ID,
                std::bind(&service_sample::on_recovery_request, this, std::placeholders::_1));

        std::set<vsomeip::eventgroup_t> its_groups;
        its_groups.insert(SAMPLE_EVENTGROUP_ID);
        app_->offer_event(
                SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID,
                its_groups, vsomeip::event_type_e::ET_FIELD, std::chrono::milliseconds::zero(),
                false, true, nullptr, vsomeip::reliability_type_e::RT_UNKNOWN);

        payload_ = vsomeip::runtime::get()->create_payload();
        return true;
    }

    void start() {
        app_->start();
    }

#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
    void stop() {
        running_ = false;
        blocked_ = true;
        condition_.notify_one();
        app_->clear_all_handler();
        app_->stop_offer_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
        app_->stop_offer_service(SAMPLE_SERVICE_ID, RECOVERY_RPC_INSTANCE_ID);
        if (std::this_thread::get_id() != offer_thread_.get_id()) {
            if (offer_thread_.joinable())
                offer_thread_.join();
        } else {
            offer_thread_.detach();
        }
        app_->stop();
    }
#endif

    void on_state(vsomeip::state_type_e _state) {
        std::cout << "Application " << app_->get_name() << " is "
                  << (_state == vsomeip::state_type_e::ST_REGISTERED ? "registered." : "deregistered.")
                  << std::endl;
        if (_state == vsomeip::state_type_e::ST_REGISTERED) {
            std::lock_guard<std::mutex> its_lock(mutex_);
            blocked_ = true;
            condition_.notify_one();
        }
    }

    // Same as response-udp-recovery.cpp's handler: answer immediately with the current
    // counter, over the UDP-only instance - never touched by the TCP-port loss injection,
    // so this RPC exchange is unaffected by whatever is happening to the TCP stream.
    void on_recovery_request(const std::shared_ptr<vsomeip::message> &_request) {
        std::shared_ptr<vsomeip::message> its_response =
                vsomeip::runtime::get()->create_response(_request);
        std::shared_ptr<vsomeip::payload> its_payload =
                vsomeip::runtime::get()->create_payload();
        vsomeip::byte_t its_data[8];
        uint64_t seq = sequence_;
        for (int i = 0; i < 8; ++i)
            its_data[i] = static_cast<vsomeip::byte_t>((seq >> ((7 - i) * 8)) & 0xFF);
        its_payload->set_data(its_data, sizeof(its_data));
        its_response->set_payload(its_payload);
        app_->send(its_response);
    }

    void run() {
        {
            std::unique_lock<std::mutex> its_lock(mutex_);
            while (!blocked_ && running_)
                condition_.wait(its_lock);
        }
        if (!running_)
            return;

        app_->offer_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
        app_->offer_service(SAMPLE_SERVICE_ID, RECOVERY_RPC_INSTANCE_ID);
        std::cout << "Publishing sequence numbers on event [1234.5678.8778] every "
                  << cycle_ << "ms over TCP (hybrid recovery RPC on instance 5679, UDP)"
                  << std::endl;

        while (running_) {
            vsomeip::byte_t its_data[8];
            for (int i = 0; i < 8; ++i)
                its_data[i] = static_cast<vsomeip::byte_t>((sequence_ >> ((7 - i) * 8)) & 0xFF);

            payload_->set_data(its_data, sizeof(its_data));
            app_->notify(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID, payload_);
            ++sequence_;

            std::this_thread::sleep_for(std::chrono::milliseconds(cycle_));
        }
    }

private:
    std::shared_ptr<vsomeip::application> app_;
    uint32_t cycle_;

    std::mutex mutex_;
    std::condition_variable condition_;
    bool blocked_;
    bool running_;

    uint64_t sequence_;
    std::shared_ptr<vsomeip::payload> payload_;

    std::thread offer_thread_;
};

#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
service_sample *its_sample_ptr(nullptr);
void handle_signal(int _signal) {
    if (its_sample_ptr != nullptr && (_signal == SIGINT || _signal == SIGTERM))
        its_sample_ptr->stop();
}
#endif

int main(int argc, char **argv) {
    uint32_t cycle = 1;

    std::string cycle_arg("--cycle");
    for (int i = 1; i < argc; i++) {
        if (cycle_arg == argv[i] && i + 1 < argc) {
            i++;
            std::stringstream converter;
            converter << argv[i];
            converter >> cycle;
        }
    }

    service_sample its_sample(cycle);
#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
    its_sample_ptr = &its_sample;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
#endif
    if (its_sample.init()) {
        its_sample.start();
        return 0;
    } else {
        return 1;
    }
}

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

// Prototype "fast recovery" service: publishes the same incrementing-counter event as
// response-tcp-recovery.cpp, but over UDP instead of TCP, and offers a second instance
// (RECOVERY_RPC_INSTANCE_ID) purely for a request/response RPC that the client uses to
// recover immediately once it notices a sequence-number gap - instead of relying on
// TCP's own kernel-level retransmission (which is what request/response-tcp-recovery.cpp
// measures). See CHANGES_THOR.md / the chat session that produced this for why: the
// ~2ms-plus application-level gap found there was isolated to somewhere inside the
// kernel's own TCP-stack processing (not the network, not vsomeip, not scheduling) -
// this tests whether that cost is TCP-specific (ACK scheduling, congestion state,
// reorder-queue management - none of which a UDP socket does) or a generic
// socket-receive-path cost that would show up here too.
//
// The two services are on deliberately separate UDP ports (see the two json configs)
// so that loss injection, which targets one UDP port, can drop the event stream without
// ever touching the recovery RPC exchange this tool is trying to measure.
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

    // The client calls this - over its own UDP port, never touched by loss injection -
    // the instant it notices a gap in the event sequence. Reply immediately with the
    // current counter so the client can resync and time the round trip. There is
    // deliberately nothing clever here: no history buffer, no re-sending the exact lost
    // value - this measures how fast a minimal "are you there, give me fresh data" RPC
    // completes, which is the fair UDP-side comparison to "how fast does the kernel's own
    // TCP retransmission complete" on the TCP side.
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
                  << cycle_ << "ms over UDP (recovery RPC on instance 5679)" << std::endl;

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

    // sequence_ is written only by run()'s own thread and read by on_recovery_request(),
    // which vsomeip dispatches on a different thread - not mutex-guarded, same as
    // response-tcp-recovery.cpp's sequence_ (a torn read of a monotonically increasing
    // counter is not a correctness problem for this tool: at worst the client's recovery
    // round trip resyncs to a value one write ahead or behind of "exactly current", which
    // does not affect the timing being measured).
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

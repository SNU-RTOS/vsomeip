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

// TCP packet-recovery test service. Publishes SAMPLE_EVENT_ID over TCP every `cycle_` ms;
// the payload is an 8-byte big-endian counter that increments by exactly one on every
// send. Continuity of this counter (or rather, the TIME between arrivals of consecutive
// values) is what request-tcp-recovery.cpp on the client side uses to detect and time
// recovery from a lost-and-retransmitted TCP segment - see tcp-recovery/README.md and
// CHANGES_THOR.md's "TCP Packet Recovery" section.
//
// Pair: request-tcp-recovery.cpp (client, run on the Orin). Loss injection normally runs
// as part of response_tcp.sh, via tcp-recovery/inject_loss.sh.
//
// Unlike notify-sample, this offers the service exactly once and keeps it offered for the
// whole run - notify-sample's periodic stop_offer/re-offer would itself create gaps in the
// event stream indistinguishable from a lost-and-recovered segment.
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

    void run() {
        {
            std::unique_lock<std::mutex> its_lock(mutex_);
            while (!blocked_ && running_)
                condition_.wait(its_lock);
        }
        if (!running_)
            return;

        app_->offer_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
        std::cout << "Publishing sequence numbers on event [1234.5678.8778] every "
                  << cycle_ << "ms over TCP" << std::endl;

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

    // blocked_ must be initialized before the thread is started.
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
    uint32_t cycle = 5; // default: 5ms, matches the measurements in CHANGES_THOR.md

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

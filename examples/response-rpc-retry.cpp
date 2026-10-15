// Copyright (C) 2014-2023 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
#include <csignal>
#endif
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

#include <vsomeip/vsomeip.hpp>

#include "sample-ids.hpp"

// RPC timeout+retry test service - no event stream at all, just answers a request/response
// method call immediately over UDP. Pairs with request-rpc-retry.cpp, which sends one
// request at a time and, if nothing comes back within a short timeout, retransmits the
// same request itself (classic UDP-RPC ARQ, no reliance on the stream-level statistical
// "is this normal jitter or a real loss" judgement that request-tcp-recovery.cpp /
// request-hybrid-recovery.cpp need - a single outstanding request has no such ambiguity,
// so the timeout can be set close to the link's actual RTT instead of several multiples
// of a cyclic baseline). See CHANGES_THOR.md for why this was worth measuring separately
// from both of those.
class service_sample {
public:
    service_sample()
        : app_(vsomeip::runtime::get()->create_application()),
          blocked_(false),
          running_(true),
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
                SAMPLE_SERVICE_ID, RPC_RETRY_INSTANCE_ID, SAMPLE_METHOD_ID,
                std::bind(&service_sample::on_request, this, std::placeholders::_1));
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
        app_->stop_offer_service(SAMPLE_SERVICE_ID, RPC_RETRY_INSTANCE_ID);
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

    // Answer immediately, every time - including for a retransmitted request carrying the
    // same logical content as the one that got lost. There is nothing to deduplicate here:
    // each request is independent (no shared state to resend "the same" answer from), so a
    // retry is just another request as far as this handler is concerned.
    void on_request(const std::shared_ptr<vsomeip::message> &_request) {
        std::shared_ptr<vsomeip::message> its_response =
                vsomeip::runtime::get()->create_response(_request);
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

        app_->offer_service(SAMPLE_SERVICE_ID, RPC_RETRY_INSTANCE_ID);
        std::cout << "RPC timeout+retry test service ready on [1234.5678], method 0421"
                  << std::endl;
    }

private:
    std::shared_ptr<vsomeip::application> app_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool blocked_;
    bool running_;
    std::thread offer_thread_;
};

#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
service_sample *its_sample_ptr(nullptr);
void handle_signal(int _signal) {
    if (its_sample_ptr != nullptr && (_signal == SIGINT || _signal == SIGTERM))
        its_sample_ptr->stop();
}
#endif

int main(int, char **) {
    service_sample its_sample;
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

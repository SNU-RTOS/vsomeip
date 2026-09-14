// Copyright (C) 2014-2023 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.
#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
#include <csignal>
#endif
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>

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
// This is an application-level estimate, not a wire-level measurement: it includes
// vsomeip's own receive-side processing (see the SD latency writeup for the size of that -
// tens to a few hundred µs) on top of the network recovery time proper. For a wire-level
// cross-check against this number, see tcp-recovery/analyze_recovery.py.
class client_sample {
public:
    client_sample(uint32_t _cycle, double _threshold)
        : app_(vsomeip::runtime::get()->create_application()),
          cycle_(_cycle),
          threshold_(_threshold),
          have_last_(false),
          last_seq_(0),
          received_(0),
          flagged_(0) {
    }

    bool init() {
        if (!app_->init()) {
            std::cerr << "Couldn't initialize application" << std::endl;
            return false;
        }
        std::cout << "TCP packet-recovery client: cycle=" << cycle_ << "ms, threshold="
                  << threshold_ << "x (recovery flagged when a gap exceeds "
                  << static_cast<uint32_t>(cycle_ * threshold_) << "ms)" << std::endl;

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
    void stop() {
        app_->clear_all_handler();
        app_->unsubscribe(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENTGROUP_ID);
        app_->release_event(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID, SAMPLE_EVENT_ID);
        app_->release_service(SAMPLE_SERVICE_ID, SAMPLE_INSTANCE_ID);
        app_->stop();
        print_summary();
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
                ++flagged_;
                int64_t recovery_us = gap_us - expected_us;
                if (recovery_us < 0)
                    recovery_us = 0;
                VSOMEIP_WARNING << "패킷 유실 복구 시간: " << recovery_us << "us"
                                << " (수신 간격=" << gap_us << "us, 정상 주기=" << expected_us
                                << "us, seq " << last_seq_ << " -> " << seq << ")";
                std::cout << "[loss] seq " << last_seq_ << " -> " << seq << ": 복구 시간 "
                          << recovery_us << "us (수신 간격 " << gap_us << "us)" << std::endl;
            }
        } else {
            have_last_ = true;
            std::cout << "첫 이벤트 수신, seq=" << seq << std::endl;
        }

        last_seq_ = seq;
        last_time_ = now;
    }

    void print_summary() {
        std::cout << "총 수신 " << received_ << "건, 유실/지연 감지 " << flagged_ << "건" << std::endl;
    }

private:
    std::shared_ptr<vsomeip::application> app_;
    uint32_t cycle_;
    double threshold_;

    bool have_last_;
    uint64_t last_seq_;
    std::chrono::time_point<std::chrono::high_resolution_clock> last_time_;

    uint64_t received_;
    uint64_t flagged_;
};

#ifndef VSOMEIP_ENABLE_SIGNAL_HANDLING
client_sample *its_sample_ptr(nullptr);
void handle_signal(int _signal) {
    if (its_sample_ptr != nullptr && (_signal == SIGINT || _signal == SIGTERM))
        its_sample_ptr->stop();
}
#endif

int main(int argc, char **argv) {
    uint32_t cycle = 5;       // must match the server's --cycle for the threshold to mean anything
    double threshold = 2.0;   // gap > threshold * cycle is flagged as a loss+recovery stall

    std::string cycle_arg("--cycle");
    std::string threshold_arg("--threshold");
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
        }
    }

    client_sample its_sample(cycle, threshold);
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

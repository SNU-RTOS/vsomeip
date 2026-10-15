// Copyright (C) 2014-2017 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef VSOMEIP_EXAMPLES_SAMPLE_IDS_HPP
#define VSOMEIP_EXAMPLES_SAMPLE_IDS_HPP

#define SAMPLE_SERVICE_ID       0x1234
#define SAMPLE_INSTANCE_ID      0x5678
#define SAMPLE_METHOD_ID        0x0421

#define SAMPLE_EVENT_ID         0x8778
#define SAMPLE_GET_METHOD_ID    0x0001
#define SAMPLE_SET_METHOD_ID    0x0002

#define SAMPLE_EVENTGROUP_ID    0x4465

#define OTHER_SAMPLE_SERVICE_ID 0x0248
#define OTHER_SAMPLE_INSTANCE_ID 0x5422
#define OTHER_SAMPLE_METHOD_ID  0x1421

// Second instance of SAMPLE_SERVICE_ID, offered alongside the first by
// response-udp-recovery.cpp: a dedicated UDP port for the recovery
// request/response RPC (SAMPLE_METHOD_ID), kept separate from the lossy
// event stream's own port so loss injection (which targets one UDP port)
// never also drops the recovery exchange it is supposed to be measuring.
#define RECOVERY_RPC_INSTANCE_ID 0x5679

// response-rpc-retry.cpp's own instance - deliberately NOT SAMPLE_INSTANCE_ID
// (0x5678): that one is also offered, differently (reliable vs unreliable,
// different port), by response-tcp-recovery.cpp/response-hybrid-recovery.cpp.
// Reusing it here caused vsomeip's SD layer to see the "same" service/instance
// change offer type between test runs and get confused (reproduced live:
// "process_offerservice_serviceentry: Remote offer type changed", flapping
// availability, zero requests ever actually sent) - a fresh instance id avoids
// the whole class of bug instead of relying on both sides never overlapping in
// time.
#define RPC_RETRY_INSTANCE_ID 0x567a

#endif // VSOMEIP_EXAMPLES_SAMPLE_IDS_HPP

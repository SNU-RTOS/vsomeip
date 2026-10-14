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

#endif // VSOMEIP_EXAMPLES_SAMPLE_IDS_HPP

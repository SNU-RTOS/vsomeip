# Copyright (C) 2015-2017 Bayerische Motoren Werke Aktiengesellschaft (BMW AG)
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

set(EXAMPLE_CONFIG_FILES
    "../config/vsomeip.json"
    "../config/vsomeip-local.json"
    "../config/vsomeip-tcp-client.json"
    "../config/vsomeip-tcp-service.json"
    "../config/vsomeip-udp-client.json"
    "../config/vsomeip-udp-service.json"
    "../config/vsomeip-udp-recovery-client.json"
    "../config/vsomeip-udp-recovery-service.json"
)

# Examples
add_executable(request-sample request-sample.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(request-sample ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_executable(request-sd request-sd.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(request-sd ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_executable(request-tcp-recovery request-tcp-recovery.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(request-tcp-recovery ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_executable(response-sample response-sample.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(response-sample ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_executable(response-sd response-sd.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(response-sd ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_executable(response-tcp-recovery response-tcp-recovery.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(response-tcp-recovery ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_executable(request-udp-recovery request-udp-recovery.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(request-udp-recovery ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_executable(response-udp-recovery response-udp-recovery.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(response-udp-recovery ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_executable(subscribe-sample subscribe-sample.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(subscribe-sample ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_executable(notify-sample notify-sample.cpp ${EXAMPLE_CONFIG_FILES})
target_link_libraries(notify-sample ${VSOMEIP_NAME} ${Boost_LIBRARIES} ${DL_LIBRARY})

add_dependencies(examples request-sample response-sample subscribe-sample notify-sample request-sd response-sd request-tcp-recovery response-tcp-recovery request-udp-recovery response-udp-recovery)

install (
    TARGETS request-sample response-sample subscribe-sample notify-sample request-sd response-sd request-tcp-recovery response-tcp-recovery request-udp-recovery response-udp-recovery
    RUNTIME DESTINATION "${INSTALL_BIN_DIR}" COMPONENT bin
)

###################################################################################################

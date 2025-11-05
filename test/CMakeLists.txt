# test2 - Rewritten tests using improved framework

# Network test infrastructure
add_library(test2_network_infra
    network/infra/simulated_network.cpp
    network/infra/simulated_node.cpp
    network/infra/node_simulator.cpp
    network/infra/network_bridged_transport.cpp
)

target_include_directories(test2_network_infra PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/test/network/infra
    ${CMAKE_SOURCE_DIR}/test/network
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(test2_network_infra PUBLIC
    network
)

# Enable test-only hooks in production headers for test infra as well
 target_compile_definitions(test2_network_infra PUBLIC COINBASECHAIN_TESTS=1)
 # Ensure network library compiles test hooks for this build of tests
 target_compile_definitions(network PUBLIC COINBASECHAIN_TESTS=1)

# Test executable
add_executable(coinbasechain_tests
    ${CMAKE_SOURCE_DIR}/test/catch_amalgamated.cpp  # Use existing Catch2
    test_logging.cpp
    network/simple_test.cpp
    network/malformed_message_tests.cpp
    network/headers_sync_tests.cpp
    network/multi_peer_sync_tests.cpp
    network/dos/invalid_pow_tests.cpp
    network/dos/orphan_spam_tests.cpp
    network/dos/oversized_headers_tests.cpp
    network/dos/low_work_headers_tests.cpp
    network/dos/stalling_peer_tests.cpp
    network/dos/overflow_compactsize_tests.cpp
    network/dos/reserve_guard_tests.cpp
    network/dos/per_ip_limit_tests.cpp
    network/dos/oversized_inv_tests.cpp
    network/dos/oversized_addr_tests.cpp
    network/dos/getheaders_oversize_tests.cpp
    network/dos/per_peer_buffer_tests.cpp
    network/dos/flood_buffer_overflow_tests.cpp
    network/dos/inv_storm_throttling_tests.cpp
    network/dos/addr_valid_flood_tests.cpp
    network/dos/addr_echo_suppression_tests.cpp
    network/dos/ping_pong_tests.cpp
    network/dos/connect_churn_tests.cpp
    network/dos/reannounce_ttl_tests.cpp
    network/block_announcement/basic_tests.cpp
    network/block_announcement/edge_case_tests.cpp
    network/block_announcement/peer_states_tests.cpp
    network/block_announcement/block_relay_comprehensive_tests.cpp
    network/block_announcement/block_relay_policy_tests.cpp
    network/handshake/edge_case_tests.cpp
    network/limits/connection_limit_tests.cpp
    network/limits/slot_exhaustion_proof.cpp
    network/misbehavior/penalty_tests.cpp
    network/peer/connection_tests.cpp
    network/peer/peer_unit_tests.cpp
    network/peer/adversarial_tests.cpp
    network/peer/peer_regression_tests.cpp
    network/peer/duplicate_connection_tests.cpp
    network/edge/edge_case_tests.cpp
    network/reorg/reorg_partition_tests.cpp
    network/sync/sync_ibd_tests.cpp
    network/nat_manager_tests.cpp
    network/invalidate/invalidateblock_tests.cpp
    network/conditions/conditions_tests.cpp
    network/scale/scale_tests.cpp
    network/peer/peer_discovery_tests.cpp
    network/banman_adversarial_tests.cpp
    network/manager/header_sync_adversarial_tests.cpp
    network/manager/header_sync_ibd_gating_tests.cpp
    network/manager/header_sync_reselection_regression_tests.cpp
    network/manager/ibd_gating_tests.cpp
    network/manager/sync_started_reset_tests.cpp
    network/manager/sync_peer_switching_tests.cpp
    chain/orphan_dos_tests.cpp
    chain/orphan_edge_case_tests.cpp
    chain/orphan_integration_tests.cpp
    chain/invalidateblock_chain_tests.cpp
    chain/stress_threading_tests.cpp
    security/security_quick_tests.cpp
    security/security_attack_simulations.cpp
    network/addr_manager_tests2.cpp
    network/feeler_anchor_tests.cpp
    network/anchors_integration_tests.cpp
    network/addr/getaddr_router_tests.cpp
    network/addr/getaddr_multi_node_tests.cpp
    network/real_transport_tests.cpp
    network/notifications_tests.cpp
    network/message_dispatcher_tests.cpp
    util/threadsafe_containers_tests.cpp
)

target_include_directories(coinbasechain_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/test/network/infra
    ${CMAKE_SOURCE_DIR}/test/network
    ${CMAKE_SOURCE_DIR}/test  # For catch_amalgamated.hpp
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(coinbasechain_tests PRIVATE
    test2_network_infra
    network
)

# Define test-only macro to enable debug hooks in production code (tests only)
 target_compile_definitions(coinbasechain_tests PRIVATE COINBASECHAIN_TESTS=1)

# Set output directory
set_target_properties(coinbasechain_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Enable testing
enable_testing()
add_test(NAME coinbasechain_tests COMMAND $<TARGET_FILE:coinbasechain_tests>)

# Include all unit tests under test/unit
file(GLOB UNIT_TEST_SOURCES "${CMAKE_SOURCE_DIR}/test/unit/*.cpp")
if(UNIT_TEST_SOURCES)
target_sources(coinbasechain_tests PRIVATE ${UNIT_TEST_SOURCES})
endif()

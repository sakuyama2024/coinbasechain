cmake_minimum_required(VERSION 3.20)

project(CoinbaseChain VERSION 0.1.0 LANGUAGES CXX C)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sanitizer options (for testing only)
# Usage: cmake -DSANITIZE=thread ..
#        cmake -DSANITIZE=address ..
#        cmake -DSANITIZE=undefined ..
option(SANITIZE "Enable sanitizer (thread, address, undefined)" "")

if(SANITIZE)
    message(STATUS "Enabling ${SANITIZE} sanitizer")

    if(SANITIZE STREQUAL "thread")
        set(SANITIZER_FLAGS "-fsanitize=thread -g -O1")
        set(SANITIZER_LINK_FLAGS "-fsanitize=thread")
    elseif(SANITIZE STREQUAL "address")
        set(SANITIZER_FLAGS "-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer -g -O1")
        set(SANITIZER_LINK_FLAGS "-fsanitize=address -fsanitize=undefined")
    elseif(SANITIZE STREQUAL "undefined")
        set(SANITIZER_FLAGS "-fsanitize=undefined -fno-omit-frame-pointer -g")
        set(SANITIZER_LINK_FLAGS "-fsanitize=undefined")
    else()
        message(FATAL_ERROR "Unknown sanitizer: ${SANITIZE}. Valid options: thread, address, undefined")
    endif()

    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZER_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZER_LINK_FLAGS}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${SANITIZER_LINK_FLAGS}")

    message(STATUS "Sanitizer flags: ${SANITIZER_FLAGS}")
endif()

# Find required packages
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS system)

# Fetch external dependencies
include(FetchContent)

# RandomX library from Unicity's fork (includes modified salt)
message(STATUS "Fetching RandomX from Unicity fork...")
FetchContent_Declare(
  randomx
  GIT_REPOSITORY https://github.com/unicitynetwork/RandomX.git
  GIT_TAG        origin/master
  GIT_SHALLOW    TRUE
)

# Disable RandomX tests and benchmarks
set(RANDOMX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RANDOMX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(randomx)

# spdlog for modern logging
message(STATUS "Fetching spdlog v1.12.0...")
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.12.0
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(spdlog)

# nlohmann/json for data serialization
message(STATUS "Fetching nlohmann/json v3.11.3...")
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(json)

# Create util library (file utilities + logging + locking + thread pool + time + timedata)
add_library(util STATIC
    src/util/files.cpp
    src/util/logging.cpp
    src/util/fs_lock.cpp
    src/util/threadpool.cpp
    src/util/time.cpp
    src/util/timedata.cpp
)

target_include_directories(util PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(util PUBLIC
    spdlog::spdlog
)

# Create crypto library (Bitcoin's crypto primitives + RandomX)
add_library(crypto STATIC
    src/crypto/sha256.cpp
    src/crypto/uint.cpp
    src/crypto/arith_uint256.cpp
    src/crypto/randomx_pow.cpp
)

target_include_directories(crypto PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(crypto PUBLIC
    randomx
    util
)

# Create primitives library (block headers)
add_library(primitives STATIC
    src/primitives/block.cpp
)

target_include_directories(primitives PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(primitives PUBLIC
    crypto
)

# Create network library
add_library(network STATIC
    src/network/protocol.cpp
    src/network/message.cpp
    src/network/peer.cpp
    src/network/addr_manager.cpp
    src/network/peer_manager.cpp
    src/network/network_manager.cpp
    src/network/real_transport.cpp
    src/network/simulated_transport.cpp
)

target_include_directories(network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(network PUBLIC
    crypto
    primitives
    chain
    sync
    util
    Boost::system
    nlohmann_json::nlohmann_json
)

# Create consensus library (PoW, difficulty adjustment)
add_library(consensus STATIC
    src/consensus/pow.cpp
)

target_include_directories(consensus PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(consensus PUBLIC
    primitives
    crypto
)

# Create validation library (block validation)
add_library(validation STATIC
    src/validation/validation.cpp
    src/validation/chainstate_manager.cpp
    src/validation/chain_selector.cpp
)

target_include_directories(validation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(validation PUBLIC
    consensus
    chain
    crypto
    notifications
    util
)

# Create notifications library (modern event system)
add_library(notifications STATIC
    src/notifications.cpp
)

target_include_directories(notifications PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(notifications PUBLIC
    primitives
)

# Create chain library (block index, chain, block manager)
add_library(chain STATIC
    src/chain/block_index.cpp
    src/chain/chain.cpp
    src/chain/block_manager.cpp
    src/chain/chainparams.cpp
)

target_include_directories(chain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(chain PUBLIC
    primitives
    crypto
    util
    nlohmann_json::nlohmann_json
)

# Create sync library (header sync + peer management + ban management)
add_library(sync STATIC
    src/sync/header_sync.cpp
    src/sync/peer_manager.cpp
    src/sync/banman.cpp
)

target_include_directories(sync PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sync PUBLIC
    chain
    primitives
    crypto
    util
    nlohmann_json::nlohmann_json
)

# Create mining library (CPU miner)
add_library(mining STATIC
    src/mining/miner.cpp
)

target_include_directories(mining PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(mining PUBLIC
    chain
    primitives
    crypto
    consensus
    validation
    notifications
    util
)

# Create RPC library
add_library(rpc STATIC
    src/rpc/rpc_server.cpp
    src/rpc/rpc_client.cpp
)

target_include_directories(rpc PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(rpc PUBLIC
    chain
    network
    mining
    util
)

# Create app library
add_library(app STATIC
    src/app/application.cpp
)

target_include_directories(app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(app PUBLIC
    network
    util
    chain
    rpc
    validation
)

# Create our executable
add_executable(coinbasechain
    src/main.cpp
)

# Include directories
target_include_directories(coinbasechain PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(coinbasechain PRIVATE
    Threads::Threads
    Boost::system
    app
)

# Set output directory
set_target_properties(coinbasechain PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Create CLI executable
add_executable(coinbasechain-cli
    src/cli.cpp
)

target_include_directories(coinbasechain-cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(coinbasechain-cli PRIVATE
    rpc
)

set_target_properties(coinbasechain-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Enable testing
enable_testing()

# Create test executable (now includes network tests - all using Catch2)
add_executable(coinbasechain_tests
    test/catch_amalgamated.cpp
    test/block_tests.cpp
    test/block_index_tests.cpp
    test/files_tests.cpp
    test/chain_tests.cpp
    test/chainparams_tests.cpp
    test/header_sync_tests.cpp
    test/persistence_tests.cpp
    test/addr_manager_tests.cpp
    test/dos_protection_tests.cpp
    test/threading_tests.cpp
    test/stress_threading_tests.cpp
    test/uint_tests.cpp
    test/validation_tests.cpp
    test/orphan_basic_tests.cpp
    test/orphan_dos_tests.cpp
    test/orphan_edge_case_tests.cpp
    test/orphan_integration_tests.cpp
    test/orphan_security_tests.cpp
    test/timedata_tests.cpp
    test/reorg_tests.cpp
    test/pow_tests.cpp
    test/invalidateblock_tests.cpp
    test/unit/reorg_multi_node_tests.cpp
    test/network/simulated_network.cpp
    test/network/simulated_node.cpp
    test/network/network_bridged_transport.cpp
    test/network/attack_simulated_node.cpp
    test/network/network_tests.cpp
    test/network/invalidateblock_functional_tests.cpp
    test/security_attack_simulations.cpp
    test/security_quick_tests.cpp
)

target_include_directories(coinbasechain_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/network
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(coinbasechain_tests PRIVATE
    primitives
    crypto
    util
    chain
    sync
    network
    validation
    consensus
    Boost::system
)

# Register test with CTest
add_test(NAME coinbasechain_tests COMMAND coinbasechain_tests)

# Add tools subdirectory
add_subdirectory(tools/genesis_miner)
add_subdirectory(tools/attack_node)

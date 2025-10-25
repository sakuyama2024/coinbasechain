cmake_minimum_required(VERSION 3.20)

project(CoinbaseChain VERSION 1.0.0 LANGUAGES CXX C)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add optimization flags to avoid consteval issues with fmt/spdlog
# Similar to what fuzz targets use - O1 helps avoid consteval strict checking
if(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE "Release")
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1")
endif()

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

# Find Boost (system component is header-only in Boost 1.73+)
# For compatibility with both old and new Boost versions, we try to find
# the system component but don't fail if it's not available as a separate library
find_package(Boost REQUIRED)
if(NOT TARGET Boost::system)
    # Create an interface target for header-only Boost.System
    add_library(Boost::system INTERFACE IMPORTED)
    set_target_properties(Boost::system PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${Boost_INCLUDE_DIRS}"
    )
endif()

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

# fmt library - use v8.1.1 (stable, compatible with spdlog v1.12.0)
# Patch to remove FMT_STRING usage in format-inl.h to avoid consteval errors
message(STATUS "Fetching fmt v8.1.1...")

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt.git
  GIT_TAG        8.1.1
  GIT_SHALLOW    TRUE
  PATCH_COMMAND sed -i.bak "2534s/FMT_STRING[(]\\\"{:x}\\\"[)]/\\\"{:x}\\\"/g" include/fmt/format-inl.h &&
                sed -i.bak "2538s/FMT_STRING[(]\\\"{:08x}\\\"[)]/\\\"{:08x}\\\"/g" include/fmt/format-inl.h
)
FetchContent_MakeAvailable(fmt)

# spdlog for modern logging - use external fmt instead of bundled
# Patch to disable SPDLOG_FMT_STRING to avoid consteval errors
message(STATUS "Fetching spdlog v1.12.0 with external fmt...")
set(SPDLOG_USE_STD_FORMAT OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)

FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.12.0
  GIT_SHALLOW    TRUE
  PATCH_COMMAND sed -i.bak "54s/.*/# define SPDLOG_FMT_STRING(format_string) format_string/" include/spdlog/common.h
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

# miniupnpc for UPnP NAT traversal
message(STATUS "Fetching miniupnpc v2.2.5...")
FetchContent_Declare(
  miniupnpc
  GIT_REPOSITORY https://github.com/miniupnp/miniupnp.git
  GIT_TAG        miniupnpc_2_2_5
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  miniupnpc
)

# Disable miniupnpc tests
set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(miniupnpc)

# ============================================================================
# LIBRARY CONSOLIDATION: 2 libraries instead of 11
# ============================================================================
#
# chain: All blockchain logic (crypto, primitives, consensus, validation, mining, util)
# network: All I/O and communication (networking, RPC, application)
#

# Create chain library - everything blockchain-related
add_library(chain STATIC
    # Util
    src/chain/files.cpp
    src/chain/logging.cpp
    src/chain/fs_lock.cpp
    src/chain/threadpool.cpp
    src/chain/time.cpp
    src/chain/timedata.cpp
    # Crypto
    src/chain/sha256.cpp
    src/chain/uint.cpp
    src/chain/arith_uint256.cpp
    src/chain/randomx_pow.cpp
    # Primitives
    src/chain/block.cpp
    # Consensus
    src/chain/pow.cpp
    # Notifications
    src/chain/notifications.cpp
    # Chain
    src/chain/block_index.cpp
    src/chain/chain.cpp
    src/chain/block_manager.cpp
    src/chain/chainparams.cpp
    # Validation
    src/chain/validation.cpp
    src/chain/chainstate_manager.cpp
    src/chain/chain_selector.cpp
    # Mining
    src/chain/miner.cpp
)

target_include_directories(chain PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Disable optimized SHA256 (SSE4/AVX2/SHANI) since we don't have implementation files
target_compile_definitions(chain PRIVATE DISABLE_OPTIMIZED_SHA256)

# Get randomx include directory after it's been made available
FetchContent_GetProperties(randomx)
if(randomx_POPULATED)
    target_include_directories(chain PUBLIC ${randomx_SOURCE_DIR}/src)
endif()

target_link_libraries(chain PUBLIC
    randomx
    spdlog::spdlog
    nlohmann_json::nlohmann_json
)

# Create network library - everything I/O and communication-related
add_library(network STATIC
    src/network/protocol.cpp
    src/network/connection_types.cpp
    src/network/message.cpp
    src/network/peer.cpp
    src/network/addr_manager.cpp
    src/network/peer_manager.cpp
    src/network/anchor_manager.cpp
    src/network/header_sync_manager.cpp
    src/network/block_relay_manager.cpp
    src/network/message_router.cpp
    src/network/network_manager.cpp
    src/network/real_transport.cpp
    # src/network/simulated_transport.cpp  # Not implemented yet
    src/network/nat_manager.cpp
    src/network/banman.cpp
    # RPC
    src/network/rpc_server.cpp
    src/network/rpc_client.cpp
)

target_include_directories(network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(network PUBLIC
    chain
    Boost::system
    nlohmann_json::nlohmann_json
    libminiupnpc-static
)

# Create our executable
add_executable(coinbasechain
    src/main.cpp
    src/application.cpp
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
    network
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
    network
)

set_target_properties(coinbasechain-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Enable testing
enable_testing()

# Option to build legacy tests under test/
option(BUILD_LEGACY_TESTS "Build legacy tests in test/" OFF)

# Create legacy test executable (Catch2) when enabled
if(BUILD_LEGACY_TESTS)
add_executable(coinbasechain_tests
    test/catch_amalgamated.cpp
    test/test_logging.cpp
    # Unit tests
    test/unit/block_tests.cpp
    test/unit/block_index_tests.cpp
    test/unit/block_manager_tests.cpp
    test/unit/files_tests.cpp
    test/unit/chain_tests.cpp
    test/unit/chain_selector_tests.cpp
    test/unit/chainstate_manager_tests.cpp
    test/unit/chainparams_tests.cpp
    test/unit/persistence_tests.cpp
    test/unit/addr_manager_tests.cpp
    test/unit/threading_tests.cpp
    test/unit/uint_tests.cpp
    test/unit/validation_tests.cpp
    test/unit/orphan_basic_tests.cpp
    test/unit/timedata_tests.cpp
    test/unit/reorg_tests.cpp
    test/unit/pow_tests.cpp
    test/unit/reorg_multi_node_tests.cpp
    test/unit/network_header_sync_tests.cpp
    test/unit/notification_tests.cpp
    test/unit/anchor_manager_tests.cpp
    test/unit/connection_types_tests.cpp
    test/unit/protocol_tests.cpp
    test/unit/banman_tests.cpp
    test/unit/message_tests.cpp
    test/unit/peer_manager_tests.cpp
    test/unit/message_router_tests.cpp
    test/unit/miner_tests.cpp
    # Integration tests
    test/integration/banman_adversarial_tests.cpp
    test/integration/header_sync_adversarial_tests.cpp
    test/integration/invalidateblock_tests.cpp
    test/integration/orphan_dos_tests.cpp
    test/integration/orphan_edge_case_tests.cpp
    test/integration/orphan_integration_tests.cpp
    test/integration/security_attack_simulations.cpp
    test/integration/security_quick_tests.cpp
    test/integration/stress_threading_tests.cpp
    test/dos_protection_tests.cpp
    # Network tests
    test/network/simulated_network.cpp
    test/network/simulated_node.cpp
    test/network/network_bridged_transport.cpp
    test/network/attack_simulated_node.cpp
    test/network/nat_manager_tests.cpp
    test/network/peer_connection_tests.cpp
    test/network/sync_ibd_tests.cpp
    test/network/reorg_partition_tests.cpp
    test/network/attack_simulation_tests.cpp
    test/network/inbound_slot_attack_proof.cpp
    test/network/misbehavior_penalty_tests.cpp
    test/network/peer_tests.cpp
    test/network/peer_adversarial_tests.cpp
    test/network/peer_discovery_tests.cpp
    test/network/invalidateblock_functional_tests.cpp
    test/network/block_announcement_tests.cpp
    test/network/block_announcement_peer_states_tests.cpp
    test/network/block_announcement_edge_case_tests.cpp
    test/network/low_work_headers_test.cpp
    test/network/permission_integration_tests.cpp
)

target_include_directories(coinbasechain_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/network
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(coinbasechain_tests PRIVATE
    chain
    network
    Boost::system
)

# Register test with CTest
add_test(NAME coinbasechain_tests COMMAND coinbasechain_tests)
endif() # BUILD_LEGACY_TESTS

# Add tools subdirectory
add_subdirectory(tools/genesis_miner)
add_subdirectory(tools/attack_node)

# Fuzzing targets (requires libFuzzer or OSS-Fuzz environment)
# Build with: cmake -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++ ..
option(ENABLE_FUZZING "Enable fuzzing targets" OFF)

if(ENABLE_FUZZING)
    message(STATUS "Building fuzz targets")

    # Check if we're in OSS-Fuzz environment
    if(DEFINED ENV{LIB_FUZZING_ENGINE})
        # OSS-Fuzz provides LIB_FUZZING_ENGINE
        set(FUZZING_ENGINE $ENV{LIB_FUZZING_ENGINE})
    else()
        # Check if libFuzzer is available (requires link test, not just compile)
        include(CheckCXXSourceCompiles)
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")

        # On macOS with Homebrew LLVM, add libc++ path
        if(APPLE AND CMAKE_CXX_COMPILER MATCHES "homebrew")
            set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++")
        endif()

        check_cxx_source_compiles("
            #include <cstdint>
            #include <cstddef>
            extern \"C\" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
                return 0;
            }
        " HAS_LIBFUZZER)

        if(HAS_LIBFUZZER)
            set(FUZZING_ENGINE "-fsanitize=fuzzer")

            # On macOS with Homebrew LLVM, we need to use Homebrew's libc++
            if(APPLE AND CMAKE_CXX_COMPILER MATCHES "homebrew")
                set(FUZZING_ENGINE "${FUZZING_ENGINE} -L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++")
            endif()

            message(STATUS "Using libFuzzer")
        else()
            # Fallback: provide standalone fuzz driver (for testing structure)
            message(WARNING "libFuzzer not available. Fuzz targets will use standalone driver.")
            message(WARNING "Install llvm/clang with fuzzer support for actual fuzzing.")
            set(FUZZING_ENGINE "")
            add_definitions(-DSTANDALONE_FUZZ_TARGET_DRIVER)
        endif()

        # Clear CMAKE_REQUIRED_FLAGS
        set(CMAKE_REQUIRED_FLAGS "")
    endif()

    # Common function to create fuzz target
    function(add_fuzz_target target_name source_file)
        # If using standalone driver, add it to sources
        if(NOT HAS_LIBFUZZER AND NOT DEFINED ENV{LIB_FUZZING_ENGINE})
            add_executable(${target_name}
                ${source_file}
                ${CMAKE_CURRENT_SOURCE_DIR}/fuzz/standalone_fuzz_driver.cpp
            )
        else()
            add_executable(${target_name} ${source_file})
        endif()

        target_include_directories(${target_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

        target_link_libraries(${target_name} PRIVATE
            chain
            network
        )

        # Add fuzzing engine (if available)
        if(FUZZING_ENGINE)
            target_link_libraries(${target_name} PRIVATE ${FUZZING_ENGINE})
        endif()

        # Optimization flags for fuzzing
        # O2 helps avoid consteval issues with spdlog on newer compilers
        target_compile_options(${target_name} PRIVATE -O2 -g)

        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz
        )
    endfunction()

    # Create fuzz targets
    add_fuzz_target(fuzz_block_header fuzz/fuzz_block_header.cpp)
    add_fuzz_target(fuzz_varint fuzz/fuzz_varint.cpp)
    add_fuzz_target(fuzz_messages fuzz/fuzz_messages.cpp)
    add_fuzz_target(fuzz_message_header fuzz/fuzz_message_header.cpp)
    add_fuzz_target(fuzz_chain_reorg fuzz/fuzz_chain_reorg.cpp)

    message(STATUS "Fuzz targets will be built in: ${CMAKE_BINARY_DIR}/fuzz")
endif()

# ============================================================================
# TESTS: New improved testing framework (moved from test2/ to test/)
# ============================================================================
add_subdirectory(test)

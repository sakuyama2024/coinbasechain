# CMake generated Testfile for 
# Source directory: /Users/mike/Code/coinbasechain
# Build directory: /Users/mike/Code/coinbasechain/build_tsan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[coinbasechain_tests]=] "/Users/mike/Code/coinbasechain/build_tsan/coinbasechain_tests")
set_tests_properties([=[coinbasechain_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/mike/Code/coinbasechain/CMakeLists.txt;375;add_test;/Users/mike/Code/coinbasechain/CMakeLists.txt;0;")
add_test([=[network_tests]=] "/Users/mike/Code/coinbasechain/build_tsan/network_tests")
set_tests_properties([=[network_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/mike/Code/coinbasechain/CMakeLists.txt;430;add_test;/Users/mike/Code/coinbasechain/CMakeLists.txt;0;")
subdirs("_deps/randomx-build")
subdirs("_deps/spdlog-build")
subdirs("_deps/json-build")
subdirs("_deps/googletest-build")
subdirs("tools/genesis_miner")
subdirs("tools/attack_node")

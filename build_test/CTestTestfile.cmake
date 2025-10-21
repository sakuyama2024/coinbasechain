# CMake generated Testfile for 
# Source directory: /Users/mike/Code/coinbasechain-docker
# Build directory: /Users/mike/Code/coinbasechain-docker/build_test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[coinbasechain_tests]=] "/Users/mike/Code/coinbasechain-docker/build_test/coinbasechain_tests")
set_tests_properties([=[coinbasechain_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/mike/Code/coinbasechain-docker/CMakeLists.txt;346;add_test;/Users/mike/Code/coinbasechain-docker/CMakeLists.txt;0;")
subdirs("_deps/randomx-build")
subdirs("_deps/fmt-build")
subdirs("_deps/spdlog-build")
subdirs("_deps/json-build")
subdirs("_deps/miniupnpc-build")
subdirs("tools/genesis_miner")
subdirs("tools/attack_node")

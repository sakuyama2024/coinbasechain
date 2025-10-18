# CMake generated Testfile for 
# Source directory: /Users/mike/Code/coinbasechain-full
# Build directory: /Users/mike/Code/coinbasechain-full/build_tsan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[coinbasechain_tests]=] "/Users/mike/Code/coinbasechain-full/build_tsan/coinbasechain_tests")
set_tests_properties([=[coinbasechain_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/Users/mike/Code/coinbasechain-full/CMakeLists.txt;451;add_test;/Users/mike/Code/coinbasechain-full/CMakeLists.txt;0;")
subdirs("_deps/randomx-build")
subdirs("_deps/fmt-build")
subdirs("_deps/spdlog-build")
subdirs("_deps/json-build")
subdirs("tools/genesis_miner")
subdirs("tools/attack_node")

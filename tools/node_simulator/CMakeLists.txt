# Attack Node - DoS testing utility

add_executable(node_simulator
    node_simulator.cpp
)

target_include_directories(node_simulator PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(node_simulator PRIVATE
    network
    Boost::system
)

set_target_properties(node_simulator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

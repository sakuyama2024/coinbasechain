# Attack Node - DoS testing utility

add_executable(attack_node
    attack_node.cpp
)

target_include_directories(attack_node PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${Boost_INCLUDE_DIRS}
)

target_link_libraries(attack_node PRIVATE
    network
    Boost::system
)

set_target_properties(attack_node PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

cmake_minimum_required(VERSION 3.24)
project(PulpG5HostEventConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(pulp-g5-host-event-consumer main.cpp)
target_compile_features(pulp-g5-host-event-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-g5-host-event-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME g5-host-event-contract COMMAND pulp-g5-host-event-consumer)
set_tests_properties(g5-host-event-contract PROPERTIES
    LABELS "examples;installed-sdk;graph;events" TIMEOUT 10)

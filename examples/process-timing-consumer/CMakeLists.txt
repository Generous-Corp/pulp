cmake_minimum_required(VERSION 3.24)
project(PulpProcessTimingConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(pulp-process-timing-consumer main.cpp)
target_compile_features(pulp-process-timing-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-process-timing-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME process-timing-contract COMMAND pulp-process-timing-consumer)
set_tests_properties(process-timing-contract PROPERTIES
    LABELS "examples;installed-sdk;graph;events" TIMEOUT 10
    PASS_REGULAR_EXPRESSION "process-timing-consumer PASS checks=[0-9]+"
    FAIL_REGULAR_EXPRESSION "process-timing-consumer FAIL")

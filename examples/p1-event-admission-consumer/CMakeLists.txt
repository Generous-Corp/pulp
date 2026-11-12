cmake_minimum_required(VERSION 3.24)
project(PulpP1EventAdmissionConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(pulp-p1-event-admission-consumer main.cpp)
target_compile_features(pulp-p1-event-admission-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-p1-event-admission-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME p1-event-admission-public-consumer COMMAND pulp-p1-event-admission-consumer)
set_tests_properties(p1-event-admission-public-consumer PROPERTIES
    LABELS "examples;installed-sdk;graph;events" TIMEOUT 10)

cmake_minimum_required(VERSION 3.24)
project(PulpProcessorRestoreConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(pulp-processor-restore-consumer main.cpp)
target_compile_features(pulp-processor-restore-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-processor-restore-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME processor-restore-consumer COMMAND pulp-processor-restore-consumer)
set_tests_properties(processor-restore-consumer PROPERTIES
    LABELS "examples;installed-sdk;host" TIMEOUT 30)

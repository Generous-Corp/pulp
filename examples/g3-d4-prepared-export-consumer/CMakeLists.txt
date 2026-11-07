cmake_minimum_required(VERSION 3.24)
project(PulpG3D4PreparedExportConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(pulp-g3-d4-prepared-export-consumer main.cpp)
target_compile_features(pulp-g3-d4-prepared-export-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-g3-d4-prepared-export-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME g3-d4-prepared-export-consumer COMMAND pulp-g3-d4-prepared-export-consumer)
set_tests_properties(g3-d4-prepared-export-consumer PROPERTIES
    LABELS "examples;installed-sdk;graph;g3;d4" TIMEOUT 10)

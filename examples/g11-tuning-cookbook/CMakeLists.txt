cmake_minimum_required(VERSION 3.24)
project(PulpGraphTuningCookbook LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host midi)

add_executable(pulp-graph-tuning-cookbook main.cpp)
target_compile_features(pulp-graph-tuning-cookbook PRIVATE cxx_std_20)
target_link_libraries(pulp-graph-tuning-cookbook PRIVATE Pulp::host Pulp::midi)

enable_testing()
add_test(NAME graph-tuning-positive COMMAND pulp-graph-tuning-cookbook)
set_tests_properties(graph-tuning-positive PROPERTIES LABELS "examples;installed-sdk;graph;tuning")
add_test(NAME graph-tuning-planted-default-a4
         COMMAND ${CMAKE_COMMAND}
                 -DPROBE=$<TARGET_FILE:pulp-graph-tuning-cookbook>
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/planted_negative.cmake)
set_tests_properties(graph-tuning-planted-default-a4
                     PROPERTIES LABELS "examples;installed-sdk;graph;tuning;negative")

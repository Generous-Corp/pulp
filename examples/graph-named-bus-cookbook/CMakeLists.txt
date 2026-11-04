cmake_minimum_required(VERSION 3.24)
project(PulpGraphNamedBusCookbook LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS format)

add_executable(pulp-graph-named-bus-cookbook main.cpp)
target_compile_features(pulp-graph-named-bus-cookbook PRIVATE cxx_std_20)
target_link_libraries(pulp-graph-named-bus-cookbook PRIVATE Pulp::format)

enable_testing()
add_test(NAME graph-named-bus-cookbook COMMAND pulp-graph-named-bus-cookbook)
set_tests_properties(graph-named-bus-cookbook PROPERTIES LABELS "examples;installed-sdk;graph")
add_test(NAME graph-named-bus-planted-swap
         COMMAND pulp-graph-named-bus-cookbook --planted-swap)
set_tests_properties(graph-named-bus-planted-swap PROPERTIES
                     WILL_FAIL TRUE LABELS "examples;installed-sdk;graph;negative")

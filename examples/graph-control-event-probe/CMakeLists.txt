cmake_minimum_required(VERSION 3.24)
project(PulpGraphControlEventProbe LANGUAGES CXX)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    find_package(Pulp CONFIG REQUIRED COMPONENTS format graph)
endif()
add_executable(pulp-graph-control-event-probe main.cpp)
target_compile_features(pulp-graph-control-event-probe PRIVATE cxx_std_20)
target_link_libraries(pulp-graph-control-event-probe PRIVATE pulp::format pulp::graph)
enable_testing()
add_test(NAME graph-control-event-probe-consumer COMMAND pulp-graph-control-event-probe)
add_test(NAME graph-control-event-probe-planted-negative
         COMMAND pulp-graph-control-event-probe --planted-missing-publication)
# Refusal-message controls: the planted flag skips process_routed, so the run
# must stop at the publication rule. PASS_REGULAR_EXPRESSION pins that exact
# rule instead of accepting any non-zero exit.
set_tests_properties(graph-control-event-probe-planted-negative
                     PROPERTIES PASS_REGULAR_EXPRESSION "FAIL rule=control-publication[\r\n]"
                                LABELS "graph;negative")
add_test(NAME graph-control-event-probe-event-planted-negative
         COMMAND pulp-graph-control-event-probe --planted-missing-event-publication)
set_tests_properties(graph-control-event-probe-event-planted-negative
                     PROPERTIES PASS_REGULAR_EXPRESSION "FAIL rule=event-publication[\r\n]"
                                LABELS "graph;negative")

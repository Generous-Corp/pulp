cmake_minimum_required(VERSION 3.24)
project(PulpGraphAgentLoop LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)

add_executable(pulp-graph-agent-loop main.cpp)
target_compile_features(pulp-graph-agent-loop PRIVATE cxx_std_20)
target_link_libraries(pulp-graph-agent-loop PRIVATE Pulp::host)
target_compile_definitions(pulp-graph-agent-loop PRIVATE
    A9_ACTION_PLAN_PATH="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/action-plan.json")

enable_testing()
add_test(NAME graph-agent-loop-consumer
         COMMAND pulp-graph-agent-loop
                 "${CMAKE_CURRENT_BINARY_DIR}/positive"
                 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/action-plan.json")

# These are planted schema mutations. Each must fail closed before any graph
# work starts, proving that a caller cannot opt the action plan into network or
# ambient-plugin execution, or silently omit a required action.
foreach(mutation IN ITEMS network-allowed plugin-allowed missing-action)
    add_test(NAME graph-agent-loop-mutation-${mutation}
             COMMAND pulp-graph-agent-loop
                     "${CMAKE_CURRENT_BINARY_DIR}/mutation-${mutation}"
                     "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mutation-${mutation}.json")
    set_tests_properties(graph-agent-loop-mutation-${mutation} PROPERTIES WILL_FAIL TRUE)
endforeach()


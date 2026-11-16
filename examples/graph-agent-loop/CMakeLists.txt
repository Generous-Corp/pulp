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
foreach(mutation IN ITEMS network-allowed plugin-allowed missing-action duplicate-key
                         escaped-duplicate-key url-string extra-step-field edit-target)
    if(mutation STREQUAL "network-allowed")
        set(expected_rule "rule=plan-network-allow")
    elseif(mutation STREQUAL "plugin-allowed")
        set(expected_rule "rule=plan-plugin-allow")
    elseif(mutation STREQUAL "missing-action")
        set(expected_rule "rule=action-catalog")
    elseif(mutation MATCHES "duplicate-key")
        set(expected_rule "rule=plan-json-parse")
    elseif(mutation STREQUAL "url-string")
        set(expected_rule "rule=plan-network-url")
    elseif(mutation STREQUAL "extra-step-field")
        set(expected_rule "rule=plan-action-field")
    else()
        set(expected_rule "rule=action-edit")
    endif()
    add_test(NAME graph-agent-loop-mutation-${mutation}
             COMMAND ${CMAKE_COMMAND}
                     -DGRAPH_AGENT_LOOP_EXECUTABLE=$<TARGET_FILE:pulp-graph-agent-loop>
                     -DGRAPH_AGENT_LOOP_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/mutation-${mutation}
                     -DGRAPH_AGENT_LOOP_PLAN=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mutation-${mutation}.json
                     -DGRAPH_AGENT_LOOP_RULE=${expected_rule}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/assert-denial.cmake)
endforeach()


# Read the receipt from the positive test's own output directory. This catches
# regressions that silently write into the shared system temporary directory.
add_test(NAME graph-agent-loop-consumer-action-log
         COMMAND ${CMAKE_COMMAND} -E cat
                 "${CMAKE_CURRENT_BINARY_DIR}/positive/a9-action-log.jsonl")
set_tests_properties(graph-agent-loop-consumer-action-log PROPERTIES
    DEPENDS graph-agent-loop-consumer
    PASS_REGULAR_EXPRESSION "pulp.a9.action-log/v1")

# The authoring glue and the plan-boundary harness each stay inside a line
# ceiling, so glue cannot move into the harness header unseen. The two controls
# are the counter's negative controls: with a limit far below the real size of
# one file it must report that file as over budget, so a counter that sees
# nothing cannot pass.
foreach(control IN ITEMS budget over-budget-control harness-over-budget-control)
    set(glue_limit 200)
    set(harness_limit 220)
    set(expect_over "-DGRAPH_AGENT_LOOP_EXPECT_OVER=1")
    if(control STREQUAL "budget")
        set(expect_over "")
    elseif(control STREQUAL "over-budget-control")
        set(glue_limit 100)
    else()
        set(harness_limit 100)
    endif()
    add_test(NAME graph-agent-loop-glue-${control}
             COMMAND ${CMAKE_COMMAND}
                     -DGRAPH_AGENT_LOOP_GLUE=${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
                     -DGRAPH_AGENT_LOOP_HARNESS=${CMAKE_CURRENT_SOURCE_DIR}/plan_boundary.hpp
                     -DGRAPH_AGENT_LOOP_GLUE_LIMIT=${glue_limit}
                     -DGRAPH_AGENT_LOOP_HARNESS_LIMIT=${harness_limit}
                     ${expect_over}
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/assert-glue-budget.cmake)
endforeach()

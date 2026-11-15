# The A9 agent design loop consumer (examples/graph-agent-loop).
#
# Its glue and harness line ceilings need no SDK, so they run here on every
# build, with both negative controls, using the example's own counter script.
set(_graph_agent_loop_dir "${CMAKE_SOURCE_DIR}/examples/graph-agent-loop")
if(EXISTS "${_graph_agent_loop_dir}/assert-glue-budget.cmake")
    foreach(_control IN ITEMS budget over-budget-control harness-over-budget-control)
        set(_glue_limit 200)
        set(_harness_limit 220)
        set(_expect_over "-DGRAPH_AGENT_LOOP_EXPECT_OVER=1")
        if(_control STREQUAL "budget")
            set(_expect_over "")
        elseif(_control STREQUAL "over-budget-control")
            set(_glue_limit 100)
        else()
            set(_harness_limit 100)
        endif()
        add_test(NAME graph-agent-loop-glue-${_control}
            COMMAND ${CMAKE_COMMAND}
                -DGRAPH_AGENT_LOOP_GLUE=${_graph_agent_loop_dir}/main.cpp
                -DGRAPH_AGENT_LOOP_HARNESS=${_graph_agent_loop_dir}/plan_boundary.hpp
                -DGRAPH_AGENT_LOOP_GLUE_LIMIT=${_glue_limit}
                -DGRAPH_AGENT_LOOP_HARNESS_LIMIT=${_harness_limit}
                ${_expect_over}
                -P ${_graph_agent_loop_dir}/assert-glue-budget.cmake)
        set_tests_properties(graph-agent-loop-glue-${_control} PROPERTIES
            LABELS "examples;graph;a9" TIMEOUT 60)
    endforeach()
endif()

# The full consumer, its planted action plans and its own copy of the budget
# tests, built against the installed SDK.
if(TARGET pulp-host AND EXISTS "${_graph_agent_loop_dir}/CMakeLists.txt")
    add_test(NAME graph-agent-loop-installed-sdk
        COMMAND ${CMAKE_COMMAND}
            -DPULP_BUILD_DIR=${CMAKE_BINARY_DIR}
            -DPULP_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DPULP_PARENT_BUILD_TYPE=$<CONFIG>
            "-DPULP_PARENT_GENERATOR=${CMAKE_GENERATOR}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_graph_agent_loop_consumer.cmake)
    set_tests_properties(graph-agent-loop-installed-sdk PROPERTIES
        LABELS "examples;installed-sdk;graph;a9;slow"
        TIMEOUT 1800)
endif()

# Counts non-blank, non-comment lines of the authoring glue (main.cpp) and the
# plan-boundary harness (plan_boundary.hpp), and enforces the glue budget.
# With GRAPH_AGENT_LOOP_EXPECT_OVER set the check inverts: it passes only when
# the glue exceeds the limit, which proves the counter sees real code.
function(count_code_lines path out_var)
    file(READ "${path}" text)
    # Neutralize list metacharacters so each source line stays one list item.
    string(REPLACE ";" "<semi>" text "${text}")
    string(REPLACE "[" "<lbracket>" text "${text}")
    string(REPLACE "]" "<rbracket>" text "${text}")
    string(REPLACE "\n" ";" lines "${text}")
    list(FILTER lines EXCLUDE REGEX "^[ \t\r]*(//.*)?$")
    list(LENGTH lines count)
    set(${out_var} ${count} PARENT_SCOPE)
endfunction()

count_code_lines("${GRAPH_AGENT_LOOP_GLUE}" glue)
count_code_lines("${GRAPH_AGENT_LOOP_HARNESS}" harness)
math(EXPR total "${glue} + ${harness}")
message(STATUS "graph-agent-loop glue=${glue} harness=${harness} total=${total} limit=${GRAPH_AGENT_LOOP_GLUE_LIMIT}")
if(glue GREATER GRAPH_AGENT_LOOP_GLUE_LIMIT)
    set(over TRUE)
else()
    set(over FALSE)
endif()
if(GRAPH_AGENT_LOOP_EXPECT_OVER)
    if(NOT over)
        message(FATAL_ERROR "counter control failed: glue ${glue} is not over ${GRAPH_AGENT_LOOP_GLUE_LIMIT}")
    endif()
elseif(over)
    message(FATAL_ERROR "glue budget exceeded: ${glue} > ${GRAPH_AGENT_LOOP_GLUE_LIMIT}")
endif()

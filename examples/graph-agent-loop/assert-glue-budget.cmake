# Counts non-blank, non-comment lines of the authoring glue (main.cpp) and the
# plan-boundary harness (plan_boundary.hpp), and enforces a ceiling on each, so
# glue moved into the harness header is caught as well as glue that grows.
# With GRAPH_AGENT_LOOP_EXPECT_OVER set the check inverts: it passes only when
# a file exceeds its limit, which proves the counter sees real code.
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
if(NOT DEFINED GRAPH_AGENT_LOOP_HARNESS_LIMIT)
    message(FATAL_ERROR "GRAPH_AGENT_LOOP_HARNESS_LIMIT is required")
endif()
message(STATUS "graph-agent-loop glue=${glue} harness=${harness} total=${total} "
               "glue_limit=${GRAPH_AGENT_LOOP_GLUE_LIMIT} harness_limit=${GRAPH_AGENT_LOOP_HARNESS_LIMIT}")
set(over FALSE)
set(reason "")
if(glue GREATER GRAPH_AGENT_LOOP_GLUE_LIMIT)
    set(over TRUE)
    string(APPEND reason " glue ${glue} > ${GRAPH_AGENT_LOOP_GLUE_LIMIT};")
endif()
if(harness GREATER GRAPH_AGENT_LOOP_HARNESS_LIMIT)
    set(over TRUE)
    string(APPEND reason " harness ${harness} > ${GRAPH_AGENT_LOOP_HARNESS_LIMIT};")
endif()
if(GRAPH_AGENT_LOOP_EXPECT_OVER)
    if(NOT over)
        message(FATAL_ERROR "counter control failed: glue ${glue} and harness ${harness} are within their limits")
    endif()
elseif(over)
    message(FATAL_ERROR "glue budget exceeded:${reason}")
endif()

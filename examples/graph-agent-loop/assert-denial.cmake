# CTest wrapper for planted negative action plans.  CTest's WILL_FAIL property
# cannot also enforce PASS_REGULAR_EXPRESSION, so validate both the nonzero
# refusal and its named rule here and return success only when both hold.
execute_process(
    COMMAND "${GRAPH_AGENT_LOOP_EXECUTABLE}" "${GRAPH_AGENT_LOOP_OUTPUT}"
            "${GRAPH_AGENT_LOOP_PLAN}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
if(result EQUAL 0)
    message(FATAL_ERROR "negative fixture unexpectedly succeeded")
endif()
string(CONCAT output "${stdout}" "${stderr}")
string(FIND "${output}" "${GRAPH_AGENT_LOOP_RULE}" rule_offset)
if(rule_offset LESS 0)
    message(FATAL_ERROR "negative fixture omitted expected rule ${GRAPH_AGENT_LOOP_RULE}: ${output}")
endif()

execute_process(
    COMMAND "${GRAPH_AGENT_LOOP_REPLAY_EXECUTABLE}" "${GRAPH_AGENT_LOOP_REPLAY_OUTPUT}"
            "${GRAPH_AGENT_LOOP_REPLAY_PLAN}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
if(result EQUAL 0)
    message(FATAL_ERROR "mutation unexpectedly passed")
endif()
string(CONCAT output "${stdout}" "${stderr}")
string(FIND "${output}" "graph-agent-loop-replay FAIL rule=${GRAPH_AGENT_LOOP_REPLAY_RULE}" offset)
if(offset LESS 0)
    message(FATAL_ERROR "mutation omitted expected rule ${GRAPH_AGENT_LOOP_REPLAY_RULE}: ${output}")
endif()

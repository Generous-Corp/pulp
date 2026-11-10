execute_process(COMMAND "${PROBE}" ${ARGUMENT}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc STREQUAL "1" OR NOT _err MATCHES "G11 refusal: (${REASON})")
    message(FATAL_ERROR "Expected oracle refusal, got ${_rc}:\n${_out}\n${_err}")
endif()
message(STATUS "G11 planted defect detected: ${ARGUMENT} ${_err}")

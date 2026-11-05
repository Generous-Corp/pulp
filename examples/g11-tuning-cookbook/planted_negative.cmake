if(NOT DEFINED PROBE)
  message(FATAL_ERROR "missing PROBE")
endif()
execute_process(COMMAND "${PROBE}" --planted-default-a4
                RESULT_VARIABLE result
                OUTPUT_VARIABLE output
                ERROR_VARIABLE error)
if(NOT result STREQUAL "42" OR NOT error MATCHES "planted-default-a4 rejected")
  message(FATAL_ERROR "negative control lost: exit=${result} stdout=${output} stderr=${error}")
endif()

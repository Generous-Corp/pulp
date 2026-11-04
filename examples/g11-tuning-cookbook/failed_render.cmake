if(NOT DEFINED PROBE)
  message(FATAL_ERROR "missing PROBE")
endif()
execute_process(COMMAND "${PROBE}" --test-failed-render
                RESULT_VARIABLE result)
if(NOT result STREQUAL "7")
  message(FATAL_ERROR "failed graph render was accepted: exit=${result}")
endif()

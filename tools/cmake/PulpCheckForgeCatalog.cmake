# Compare the entire current runtime projection with the artifact installed
# by this build. Merely finding a GPU mode cannot establish catalog equality.
if(NOT DEFINED EXPORTER OR NOT DEFINED EXPECTED OR NOT EXISTS "${EXPECTED}")
    message(FATAL_ERROR "Build the Forge SDK catalog before checking its projection")
endif()
execute_process(COMMAND "${EXPORTER}" forge catalog export --json
    OUTPUT_VARIABLE _actual ERROR_VARIABLE _error
    RESULT_VARIABLE _result TIMEOUT 60)
if(NOT _result STREQUAL "0")
    message(FATAL_ERROR "Forge catalog exporter failed (${_result}): ${_error}")
endif()
file(READ "${EXPECTED}" _expected)
if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "Generated Forge catalog differs from the current runtime projection")
endif()
message(STATUS "Forge catalog snapshot is current")

set(_marker "PULP_INSPECT_RUNTIME_EVAL_HIGH_RISK_COMPONENT_V1")

if(NOT EXISTS "${EVAL_ARCHIVE}")
    message(FATAL_ERROR "runtime-eval archive missing: ${EVAL_ARCHIVE}")
endif()
file(STRINGS "${EVAL_ARCHIVE}" _eval_strings REGEX "${_marker}")
if(NOT _eval_strings)
    message(FATAL_ERROR "runtime-eval marker missing from ${EVAL_ARCHIVE}")
endif()

foreach(_base IN ITEMS
        "${BASE_INSPECT}"
        "${BASE_RUNTIME}"
        "${BASE_PROTOCOL}"
        "${BASE_CLIENT}"
        "${BASE_FORMAT}")
    if(NOT EXISTS "${_base}")
        message(FATAL_ERROR "base archive missing: ${_base}")
    endif()
    file(STRINGS "${_base}" _base_strings REGEX "${_marker}")
    if(_base_strings)
        message(FATAL_ERROR "runtime-eval marker leaked into base archive: ${_base}")
    endif()
endforeach()

message(STATUS "runtime-eval archive marker boundary is intact")

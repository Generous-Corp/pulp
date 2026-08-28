if(NOT DEFINED PULP_CLI OR NOT DEFINED ARTIFACT_ROOT)
    message(FATAL_ERROR "PULP_CLI and ARTIFACT_ROOT are required")
endif()

file(REMOVE_RECURSE "${ARTIFACT_ROOT}")

execute_process(
    COMMAND "${PULP_CLI}" gpu probe
        --recipe gpu-compute.magnitude.v1
        --artifacts "${ARTIFACT_ROOT}/positive"
        --json
    RESULT_VARIABLE positive_rc
    OUTPUT_VARIABLE positive_json
    ERROR_VARIABLE positive_stderr)
if(NOT positive_rc EQUAL 0)
    message(FATAL_ERROR "positive probe failed (${positive_rc}): ${positive_stderr}")
endif()
string(JSON positive_verdict ERROR_VARIABLE positive_json_error
    GET "${positive_json}" verdict)
if(positive_json_error OR NOT positive_verdict STREQUAL "pass")
    message(FATAL_ERROR "positive probe did not emit a pass result: ${positive_json_error}")
endif()
foreach(artifact IN ITEMS input.complex-f32 expected.f32 observed.f32)
    if(NOT EXISTS "${ARTIFACT_ROOT}/positive/${artifact}")
        message(FATAL_ERROR "positive probe omitted ${artifact}")
    endif()
endforeach()

execute_process(
    COMMAND "${PULP_CLI}" gpu probe
        --recipe gpu-compute.magnitude.v1
        --artifacts "${ARTIFACT_ROOT}/negative"
        --negative-control
        --json
    RESULT_VARIABLE negative_rc
    OUTPUT_VARIABLE negative_json
    ERROR_VARIABLE negative_stderr)
if(NOT negative_rc EQUAL 1)
    message(FATAL_ERROR
        "negative control must fail with exit 1, got ${negative_rc}: ${negative_stderr}")
endif()
string(JSON negative_verdict ERROR_VARIABLE negative_json_error
    GET "${negative_json}" verdict)
if(negative_json_error OR NOT negative_verdict STREQUAL "fail")
    message(FATAL_ERROR "negative control was not detected: ${negative_json_error}")
endif()

execute_process(
    COMMAND "${PULP_CLI}" gpu probe
        --recipe not-a-recipe
        --artifacts "${ARTIFACT_ROOT}/unknown"
    RESULT_VARIABLE unknown_rc
    OUTPUT_VARIABLE unknown_stdout
    ERROR_VARIABLE unknown_stderr)
if(NOT unknown_rc EQUAL 2)
    message(FATAL_ERROR "unknown recipe must fail with usage exit 2, got ${unknown_rc}")
endif()
if(EXISTS "${ARTIFACT_ROOT}/unknown")
    message(FATAL_ERROR "unknown recipe unexpectedly created artifacts")
endif()

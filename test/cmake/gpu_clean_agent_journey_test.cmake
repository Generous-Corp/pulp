if(NOT DEFINED PULP_CLI OR NOT DEFINED PYTHON OR
   NOT DEFINED JOURNEY_SCRIPT OR NOT DEFINED WORKSPACE)
    message(FATAL_ERROR "PULP_CLI, PYTHON, JOURNEY_SCRIPT, and WORKSPACE are required")
endif()

# CTest reruns own this exact build-tree path.  Removing it here preserves the
# public driver's safer contract that a caller-selected workspace must be new.
get_filename_component(workspace_name "${WORKSPACE}" NAME)
get_filename_component(workspace_parent "${WORKSPACE}" DIRECTORY)
if(NOT workspace_name STREQUAL "gpu-clean-agent-journey-workspace" OR
   workspace_parent STREQUAL "" OR workspace_parent STREQUAL "/")
    message(FATAL_ERROR "refusing unsafe clean-agent test workspace: ${WORKSPACE}")
endif()
file(REMOVE_RECURSE "${WORKSPACE}")
file(MAKE_DIRECTORY "${workspace_parent}")

execute_process(
    COMMAND "${PYTHON}" "${JOURNEY_SCRIPT}"
        --pulp "${PULP_CLI}"
        --symptom compute-readback-mismatch
        --workspace "${WORKSPACE}"
        --json
    RESULT_VARIABLE journey_rc
    OUTPUT_VARIABLE journey_json
    ERROR_VARIABLE journey_stderr)
if(NOT journey_rc EQUAL 0)
    message(FATAL_ERROR
        "clean-agent GPU journey failed (${journey_rc}): ${journey_stderr}")
endif()

string(JSON journey_schema ERROR_VARIABLE journey_json_error
    GET "${journey_json}" schema)
if(journey_json_error OR
   NOT journey_schema STREQUAL "pulp.gpu-clean-agent-journey.v1")
    message(FATAL_ERROR "clean-agent journey omitted its typed receipt")
endif()

string(JSON selected_recipe GET "${journey_json}" selection recipe_id)
string(JSON diagnosis_code GET "${journey_json}" seeded_failure diagnosis code)
string(JSON repaired_verdict GET "${journey_json}" repaired_proof verdict)
if(selected_recipe STREQUAL "" OR diagnosis_code STREQUAL "" OR
   NOT repaired_verdict STREQUAL "pass")
    message(FATAL_ERROR
        "clean-agent journey did not select, diagnose, and repair a real recipe")
endif()

if(NOT EXISTS "${WORKSPACE}/clean-agent-journey.json" OR
   NOT EXISTS "${WORKSPACE}/reference-result.json" OR
   NOT EXISTS "${WORKSPACE}/seeded-failure-result.json" OR
   NOT EXISTS "${WORKSPACE}/repaired-result.json" OR
   NOT EXISTS "${WORKSPACE}/artifacts/reference" OR
   NOT EXISTS "${WORKSPACE}/artifacts/seeded-failure" OR
   NOT EXISTS "${WORKSPACE}/artifacts/repaired")
    message(FATAL_ERROR "clean-agent journey omitted durable local evidence")
endif()

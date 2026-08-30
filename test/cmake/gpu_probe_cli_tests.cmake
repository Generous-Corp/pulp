if(NOT DEFINED PULP_CLI OR NOT DEFINED ARTIFACT_ROOT)
    message(FATAL_ERROR "PULP_CLI and ARTIFACT_ROOT are required")
endif()

file(REMOVE_RECURSE "${ARTIFACT_ROOT}")
file(MAKE_DIRECTORY "${ARTIFACT_ROOT}")

execute_process(
    COMMAND "${PULP_CLI}" gpu recipes list --json
    RESULT_VARIABLE catalog_rc
    OUTPUT_VARIABLE catalog_json
    ERROR_VARIABLE catalog_stderr)
if(NOT catalog_rc EQUAL 0)
    message(FATAL_ERROR "GPU recipe discovery failed (${catalog_rc}): ${catalog_stderr}")
endif()
foreach(recipe_id IN ITEMS
        renderer3d.hardcoded-cube.v1
        gpu-compute.magnitude.v1
        gpu-audio.stft.v1
        threejs.multi-pass.v1)
    string(FIND "${catalog_json}" "${recipe_id}" recipe_position)
    if(recipe_position EQUAL -1)
        message(FATAL_ERROR "canonical discovery omitted ${recipe_id}")
    endif()
endforeach()

execute_process(
    COMMAND "${PULP_CLI}" gpu probe --help
    RESULT_VARIABLE probe_help_rc
    OUTPUT_VARIABLE probe_help)
if(NOT probe_help_rc EQUAL 0)
    message(FATAL_ERROR "GPU probe help failed (${probe_help_rc})")
endif()
foreach(recipe_id IN ITEMS
        renderer3d.hardcoded-cube.v1
        gpu-compute.magnitude.v1
        gpu-audio.stft.v1
        threejs.multi-pass.v1)
    string(FIND "${probe_help}" "${recipe_id}" recipe_position)
    if(recipe_position EQUAL -1)
        message(FATAL_ERROR "native probe help omitted ${recipe_id}")
    endif()
endforeach()

string(JSON catalog_recipe_count LENGTH "${catalog_json}" recipes)
math(EXPR catalog_recipe_last "${catalog_recipe_count} - 1")
set(threejs_callable TRUE)
foreach(recipe_index RANGE 0 ${catalog_recipe_last})
    string(JSON candidate_recipe_id GET "${catalog_json}" recipes ${recipe_index} id)
    if(candidate_recipe_id STREQUAL "threejs.multi-pass.v1")
        string(JSON threejs_callable GET "${catalog_json}" recipes ${recipe_index} callable)
    endif()
endforeach()
if(NOT threejs_callable)
    execute_process(
        COMMAND "${PULP_CLI}" gpu probe
            --recipe threejs.multi-pass.v1
            --artifacts "${ARTIFACT_ROOT}/threejs-unavailable"
            --json
        RESULT_VARIABLE threejs_unavailable_rc
        OUTPUT_VARIABLE threejs_unavailable_json
        ERROR_VARIABLE threejs_unavailable_stderr)
    if(NOT threejs_unavailable_rc EQUAL 2)
        message(FATAL_ERROR
            "unavailable Three.js probe must return exit 2, got "
            "${threejs_unavailable_rc}: ${threejs_unavailable_stderr}")
    endif()
    string(JSON threejs_unavailable_verdict ERROR_VARIABLE threejs_json_error
        GET "${threejs_unavailable_json}" verdict)
    if(threejs_json_error OR NOT threejs_unavailable_verdict STREQUAL "unavailable")
        message(FATAL_ERROR
            "unavailable Three.js probe did not emit typed v1 evidence: "
            "${threejs_json_error}")
    endif()
endif()

execute_process(
    COMMAND "${PULP_CLI}" gpu recipes list
        --symptom compute-readback-mismatch --json
    RESULT_VARIABLE symptom_rc
    OUTPUT_VARIABLE symptom_json)
if(NOT symptom_rc EQUAL 0 OR
   NOT symptom_json MATCHES "gpu-compute.magnitude.v1" OR
   symptom_json MATCHES "renderer3d.hardcoded-cube.v1")
    message(FATAL_ERROR "symptom discovery did not return the exact recipe")
endif()

execute_process(
    COMMAND "${PULP_CLI}" gpu recipes list --symptom not-a-symptom --json
    RESULT_VARIABLE unknown_symptom_rc)
if(NOT unknown_symptom_rc EQUAL 2)
    message(FATAL_ERROR "unknown symptom must return exit 2, got ${unknown_symptom_rc}")
endif()

execute_process(
    COMMAND "${PULP_CLI}" gpu recipes list --symptom "" --json
    RESULT_VARIABLE empty_symptom_rc)
if(NOT empty_symptom_rc EQUAL 2)
    message(FATAL_ERROR "empty symptom must return exit 2, got ${empty_symptom_rc}")
endif()

execute_process(
    COMMAND "${PULP_CLI}" gpu recipes show not-a-recipe --json
    RESULT_VARIABLE unknown_show_rc)
if(NOT unknown_show_rc EQUAL 2)
    message(FATAL_ERROR "unknown catalog recipe must return exit 2, got ${unknown_show_rc}")
endif()

if(PULP_ENABLE_PROJECT_PACKAGE)
set(scaffold_dir "${ARTIFACT_ROOT}/workspace")
execute_process(
    COMMAND "${PULP_CLI}" gpu recipes scaffold gpu-compute.magnitude.v1
        --output "${scaffold_dir}" --json
    RESULT_VARIABLE scaffold_rc
    OUTPUT_VARIABLE scaffold_json
    ERROR_VARIABLE scaffold_stderr)
if(NOT scaffold_rc EQUAL 0 OR
   NOT EXISTS "${scaffold_dir}/gpu-recipe.json" OR
   NOT EXISTS "${scaffold_dir}/README.md" OR
   NOT IS_DIRECTORY "${scaffold_dir}/artifacts")
    message(FATAL_ERROR "recipe scaffold failed (${scaffold_rc}): ${scaffold_stderr}")
endif()
file(READ "${scaffold_dir}/gpu-recipe.json" scaffold_receipt)
string(JSON scaffold_receipt_schema ERROR_VARIABLE scaffold_receipt_error
    GET "${scaffold_receipt}" schema)
if(scaffold_receipt_error OR
   NOT scaffold_receipt_schema STREQUAL "pulp.gpu-recipe-selection.v1")
    message(FATAL_ERROR "recipe scaffold did not write a versioned selection receipt")
endif()
execute_process(
    COMMAND "${PULP_CLI}" gpu recipes scaffold gpu-compute.magnitude.v1
        --output "${scaffold_dir}"
    RESULT_VARIABLE existing_scaffold_rc)
if(NOT existing_scaffold_rc EQUAL 1)
    message(FATAL_ERROR "existing scaffold destination must return exit 1")
endif()

execute_process(
    COMMAND "${PULP_CLI}" gpu recipes scaffold gpu-compute.magnitude.v1
        --output "${ARTIFACT_ROOT}/trailing-workspace/"
    RESULT_VARIABLE trailing_scaffold_rc)
if(NOT trailing_scaffold_rc EQUAL 0)
    message(FATAL_ERROR "scaffold destination with a trailing separator must succeed")
endif()

execute_process(
    COMMAND "${PULP_CLI}" gpu recipes scaffold gpu-compute.magnitude.v1
        --output "${ARTIFACT_ROOT}/./dot-workspace"
    RESULT_VARIABLE dot_scaffold_rc)
if(NOT dot_scaffold_rc EQUAL 1)
    message(FATAL_ERROR "scaffold path with a dot component must return exit 1")
endif()
endif()

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

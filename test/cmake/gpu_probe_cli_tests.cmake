if(NOT DEFINED PULP_CLI OR NOT DEFINED ARTIFACT_ROOT)
    message(FATAL_ERROR "PULP_CLI and ARTIFACT_ROOT are required")
endif()

function(is_gpu_compute_adapter_unavailable result_json output_variable)
    set(is_expected_unavailable FALSE)
    string(JSON result_schema ERROR_VARIABLE result_schema_error
        GET "${result_json}" schema)
    string(JSON result_verdict ERROR_VARIABLE result_verdict_error
        GET "${result_json}" verdict)
    string(JSON result_code ERROR_VARIABLE result_code_error
        GET "${result_json}" passes 0 code)
    if(NOT result_schema_error AND NOT result_verdict_error AND NOT result_code_error AND
       result_schema STREQUAL "pulp.gpu-probe-result.v1" AND
       result_verdict STREQUAL "unavailable" AND
       result_code STREQUAL "gpu_compute_adapter_unavailable")
        set(is_expected_unavailable TRUE)
    endif()
    set(${output_variable} ${is_expected_unavailable} PARENT_SCOPE)
endfunction()

# Keep the narrow headless-Linux allowance fail-closed: another typed exit-2
# result (for example, a runtime failure) must not impersonate adapter absence.
set(adapter_unavailable_fixture
    [[{"schema":"pulp.gpu-probe-result.v1","verdict":"unavailable","passes":[{"code":"gpu_compute_adapter_unavailable"}]}]])
is_gpu_compute_adapter_unavailable("${adapter_unavailable_fixture}"
    exact_adapter_unavailable_fixture)
string(REPLACE "gpu_compute_adapter_unavailable" "probe_runtime_failed"
    unrelated_exit_two_fixture "${adapter_unavailable_fixture}")
is_gpu_compute_adapter_unavailable("${unrelated_exit_two_fixture}"
    unrelated_exit_two_accepted)
if(NOT exact_adapter_unavailable_fixture OR unrelated_exit_two_accepted)
    message(FATAL_ERROR "adapter-unavailable result classifier is not fail-closed")
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
    string(JSON candidate_recipe_id GET
        "${catalog_json}" recipes ${recipe_index} recipe id)
    if(candidate_recipe_id STREQUAL "threejs.multi-pass.v1")
        string(JSON threejs_callable GET "${catalog_json}" recipes ${recipe_index} callable)
    endif()
endforeach()

get_filename_component(build_cursor "${PULP_CLI}" DIRECTORY)
set(pulp_build_cache "")
foreach(parent_index RANGE 0 4)
    if(EXISTS "${build_cursor}/CMakeCache.txt")
        set(pulp_build_cache "${build_cursor}/CMakeCache.txt")
        break()
    endif()
    get_filename_component(build_cursor "${build_cursor}" DIRECTORY)
endforeach()
if(NOT pulp_build_cache)
    message(FATAL_ERROR "could not locate the CLI build cache")
endif()
file(STRINGS "${pulp_build_cache}" pulp_js_engine_line
    REGEX "^PULP_JS_ENGINE:[^=]*=")
if(NOT pulp_js_engine_line)
    message(FATAL_ERROR "CLI build cache does not declare PULP_JS_ENGINE")
endif()
if(NOT pulp_js_engine_line MATCHES "=v8$" AND threejs_callable)
    message(FATAL_ERROR
        "non-V8 CLI build advertised threejs.multi-pass.v1 as callable")
endif()
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
string(JSON positive_schema ERROR_VARIABLE positive_schema_error
    GET "${positive_json}" schema)
string(JSON positive_verdict ERROR_VARIABLE positive_verdict_error
    GET "${positive_json}" verdict)
set(positive_adapter_unavailable FALSE)
if(positive_schema_error OR positive_verdict_error OR
   NOT positive_schema STREQUAL "pulp.gpu-probe-result.v1")
    message(FATAL_ERROR "positive probe did not emit typed v1 evidence")
endif()
if(positive_rc EQUAL 0)
    if(NOT positive_verdict STREQUAL "pass")
        message(FATAL_ERROR "successful positive probe did not emit a pass result")
    endif()
    foreach(artifact IN ITEMS input.complex-f32 expected.f32 observed.f32)
        if(NOT EXISTS "${ARTIFACT_ROOT}/positive/${artifact}")
            message(FATAL_ERROR "positive probe omitted ${artifact}")
        endif()
    endforeach()
elseif(UNIX AND NOT APPLE AND positive_rc EQUAL 2)
    is_gpu_compute_adapter_unavailable("${positive_json}"
        positive_adapter_unavailable)
    if(NOT positive_adapter_unavailable)
        message(FATAL_ERROR
            "headless Linux probe did not emit exact typed unavailable evidence: "
            "${positive_stderr}")
    endif()
else()
    message(FATAL_ERROR "positive probe failed (${positive_rc}): ${positive_stderr}")
endif()

file(WRITE "${ARTIFACT_ROOT}/blocked-artifact-directory" "not a directory")
execute_process(
    COMMAND "${PULP_CLI}" gpu probe
        --recipe gpu-compute.magnitude.v1
        --artifacts "${ARTIFACT_ROOT}/blocked-artifact-directory/child"
        --json
    RESULT_VARIABLE publication_failure_rc
    OUTPUT_VARIABLE publication_failure_json
    ERROR_VARIABLE publication_failure_stderr)
if(NOT publication_failure_rc EQUAL 2)
    message(FATAL_ERROR
        "artifact publication failure must be unverified exit 2, got "
        "${publication_failure_rc}: ${publication_failure_stderr}")
endif()
string(JSON publication_failure_schema ERROR_VARIABLE publication_failure_json_error
    GET "${publication_failure_json}" schema)
string(JSON publication_failure_verdict ERROR_VARIABLE publication_failure_verdict_error
    GET "${publication_failure_json}" verdict)
string(JSON publication_failure_code ERROR_VARIABLE publication_failure_code_error
    GET "${publication_failure_json}" passes 0 code)
if(publication_failure_json_error OR publication_failure_verdict_error OR
   publication_failure_code_error OR
   NOT publication_failure_schema STREQUAL "pulp.gpu-probe-result.v1" OR
   NOT publication_failure_verdict STREQUAL "unverified" OR
   NOT publication_failure_code STREQUAL "artifact_publication_failed")
    message(FATAL_ERROR
        "artifact publication failure did not emit typed unverified v1 evidence")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env PULP_GPU_PROBE_TEST_FAULT=runtime
        "${PULP_CLI}" gpu probe
        --recipe gpu-compute.magnitude.v1
        --artifacts "${ARTIFACT_ROOT}/runtime-failure"
        --json
    RESULT_VARIABLE runtime_failure_rc
    OUTPUT_VARIABLE runtime_failure_json
    ERROR_VARIABLE runtime_failure_stderr)
if(NOT runtime_failure_rc EQUAL 2)
    message(FATAL_ERROR
        "runtime failure must be unverified exit 2, got "
        "${runtime_failure_rc}: ${runtime_failure_stderr}")
endif()
string(JSON runtime_failure_schema ERROR_VARIABLE runtime_failure_schema_error
    GET "${runtime_failure_json}" schema)
string(JSON runtime_failure_verdict ERROR_VARIABLE runtime_failure_json_error
    GET "${runtime_failure_json}" verdict)
string(JSON runtime_failure_code ERROR_VARIABLE runtime_failure_code_error
    GET "${runtime_failure_json}" passes 0 code)
if(runtime_failure_schema_error OR runtime_failure_json_error OR runtime_failure_code_error OR
   NOT runtime_failure_schema STREQUAL "pulp.gpu-probe-result.v1" OR
   NOT runtime_failure_verdict STREQUAL "unverified" OR
   NOT runtime_failure_code STREQUAL "probe_runtime_failed")
    message(FATAL_ERROR "runtime failure did not emit typed unverified v1 evidence")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env PULP_GPU_PROBE_TEST_FAULT=result-validation
        "${PULP_CLI}" gpu probe
        --recipe gpu-compute.magnitude.v1
        --artifacts "${ARTIFACT_ROOT}/result-validation-failure"
        --json
    RESULT_VARIABLE result_validation_failure_rc
    OUTPUT_VARIABLE result_validation_failure_json
    ERROR_VARIABLE result_validation_failure_stderr)
if(NOT result_validation_failure_rc EQUAL 2)
    message(FATAL_ERROR
        "result-validation failure must be unverified exit 2, got "
        "${result_validation_failure_rc}: ${result_validation_failure_stderr}")
endif()
string(JSON result_validation_failure_schema
    ERROR_VARIABLE result_validation_failure_schema_error
    GET "${result_validation_failure_json}" schema)
string(JSON result_validation_failure_verdict
    ERROR_VARIABLE result_validation_failure_json_error
    GET "${result_validation_failure_json}" verdict)
string(JSON result_validation_failure_code
    ERROR_VARIABLE result_validation_failure_code_error
    GET "${result_validation_failure_json}" passes 0 code)
if(result_validation_failure_schema_error OR result_validation_failure_json_error OR
   result_validation_failure_code_error OR
   NOT result_validation_failure_schema STREQUAL "pulp.gpu-probe-result.v1" OR
   NOT result_validation_failure_verdict STREQUAL "unverified" OR
   NOT result_validation_failure_code STREQUAL "probe_result_validation_failed")
    message(FATAL_ERROR
        "result-validation failure did not emit typed unverified v1 evidence")
endif()

execute_process(
    COMMAND "${PULP_CLI}" gpu probe
        --recipe gpu-compute.magnitude.v1
        --artifacts "${ARTIFACT_ROOT}/negative"
        --negative-control
        --json
    RESULT_VARIABLE negative_rc
    OUTPUT_VARIABLE negative_json
    ERROR_VARIABLE negative_stderr)
string(JSON negative_schema ERROR_VARIABLE negative_schema_error
    GET "${negative_json}" schema)
string(JSON negative_verdict ERROR_VARIABLE negative_verdict_error
    GET "${negative_json}" verdict)
if(negative_rc EQUAL 1)
    if(negative_schema_error OR negative_verdict_error OR
       NOT negative_schema STREQUAL "pulp.gpu-probe-result.v1" OR
       NOT negative_verdict STREQUAL "fail")
        message(FATAL_ERROR
            "negative control was not detected with typed fail evidence: "
            "${negative_verdict_error}")
    endif()
elseif(UNIX AND NOT APPLE AND negative_rc EQUAL 2)
    is_gpu_compute_adapter_unavailable("${negative_json}"
        negative_adapter_unavailable)
    if(NOT positive_adapter_unavailable OR NOT negative_adapter_unavailable)
        message(FATAL_ERROR
            "headless Linux negative control may be unavailable only when the "
            "positive probe established the same exact adapter-unavailable "
            "state: ${negative_stderr}")
    endif()
else()
    message(FATAL_ERROR
        "negative control must fail with exit 1, got ${negative_rc}: ${negative_stderr}")
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

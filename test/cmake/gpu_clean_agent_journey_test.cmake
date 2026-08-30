function(pulp_gpu_clean_agent_contract_mode output build_configuration
         selected_build_configuration generator_is_multi_config)
    if(NOT generator_is_multi_config STREQUAL "TRUE" AND
       NOT generator_is_multi_config STREQUAL "FALSE")
        message(FATAL_ERROR "generator multi-config metadata must be TRUE or FALSE")
    endif()
    if(generator_is_multi_config STREQUAL "TRUE")
        if(NOT build_configuration STREQUAL "Release")
            message(FATAL_ERROR
                "multi-config clean-agent contract must use explicit Release")
        endif()
        if(selected_build_configuration STREQUAL "Release")
            set(mode journey)
        else()
            set(mode multi-config-not-selected)
        endif()
    elseif(build_configuration STREQUAL "Release")
        set(mode journey)
    else()
        set(mode single-config-rejection)
    endif()
    set(${output} "${mode}" PARENT_SCOPE)
endfunction()

function(pulp_gpu_clean_agent_prepare_outcome output result stderr)
    string(STRIP "${stderr}" normalized_stderr)
    if(result EQUAL 0)
        set(outcome prepared)
    elseif(result EQUAL 1 AND
            normalized_stderr STREQUAL
                "FAIL: acceptance requires authentic hardware adapter evidence")
        set(outcome hardware-nonterminal)
    else()
        set(outcome failure)
    endif()
    set(${output} "${outcome}" PARENT_SCOPE)
endfunction()

if(NOT DEFINED BUILD_CONFIGURATION OR
   NOT DEFINED SELECTED_BUILD_CONFIGURATION OR
   NOT DEFINED GENERATOR_IS_MULTI_CONFIG)
    message(FATAL_ERROR
        "build configuration, selected configuration, and generator metadata are required")
endif()
pulp_gpu_clean_agent_contract_mode(
    contract_mode "${BUILD_CONFIGURATION}" "${SELECTED_BUILD_CONFIGURATION}"
    "${GENERATOR_IS_MULTI_CONFIG}")
if(DEFINED CONFIGURATION_POLICY_ONLY AND CONFIGURATION_POLICY_ONLY)
    if(NOT DEFINED EXPECTED_CONFIGURATION_MODE OR
       NOT contract_mode STREQUAL EXPECTED_CONFIGURATION_MODE)
        message(FATAL_ERROR
            "configuration policy expected ${EXPECTED_CONFIGURATION_MODE}, got ${contract_mode}")
    endif()
    message(STATUS "gpu_clean_agent_configuration_mode=${contract_mode}")
    return()
endif()
if(DEFINED PREPARE_OUTCOME_POLICY_ONLY AND PREPARE_OUTCOME_POLICY_ONLY)
    if(NOT DEFINED PREPARE_RESULT OR NOT DEFINED PREPARE_STDERR OR
       NOT DEFINED EXPECTED_PREPARE_OUTCOME)
        message(FATAL_ERROR "prepare outcome policy inputs are required")
    endif()
    pulp_gpu_clean_agent_prepare_outcome(
        prepare_outcome "${PREPARE_RESULT}" "${PREPARE_STDERR}")
    if(NOT prepare_outcome STREQUAL EXPECTED_PREPARE_OUTCOME)
        message(FATAL_ERROR
            "prepare outcome expected ${EXPECTED_PREPARE_OUTCOME}, got ${prepare_outcome}")
    endif()
    message(STATUS "gpu_clean_agent_prepare_outcome=${prepare_outcome}")
    return()
endif()
if(contract_mode STREQUAL "multi-config-not-selected")
    message(FATAL_ERROR
        "multi-config clean-agent contract must be registered only for Release")
endif()

if(NOT DEFINED PULP_CLI OR NOT DEFINED CLI_INSTALL_SCRIPT OR
   NOT DEFINED WEBGPU_RUNTIME_LIB OR
   NOT DEFINED PYTHON OR NOT DEFINED JOURNEY_SCRIPT OR NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED BUILD_ROOT)
    message(FATAL_ERROR
        "built CLI, install script, runtime libraries, Python, journey, source, and build are required")
endif()
if(NOT EXISTS "${PULP_CLI}" OR NOT EXISTS "${CLI_INSTALL_SCRIPT}" OR
   NOT EXISTS "${WEBGPU_RUNTIME_LIB}")
    message(FATAL_ERROR "built CLI installation inputs are unavailable")
endif()
if(DEFINED V8_RUNTIME_LIBRARY AND NOT V8_RUNTIME_LIBRARY STREQUAL "" AND
   NOT EXISTS "${V8_RUNTIME_LIBRARY}")
    message(FATAL_ERROR "configured V8 runtime is unavailable")
endif()

set(temp_candidate "$ENV{TMPDIR}")
if(temp_candidate STREQUAL "")
    set(temp_candidate "$ENV{TEMP}")
endif()
if(temp_candidate STREQUAL "")
    set(temp_candidate "$ENV{TMP}")
endif()
if(temp_candidate STREQUAL "" AND EXISTS "/tmp")
    set(temp_candidate "/tmp")
endif()
get_filename_component(temp_root "${temp_candidate}" REALPATH)
if(temp_root STREQUAL "" OR temp_root STREQUAL "/")
    message(FATAL_ERROR "refusing unsafe temporary root")
endif()
string(SHA256 test_id "${CMAKE_CURRENT_BINARY_DIR}")
set(test_root "${temp_root}/pulp-gpu-clean-agent-preparer-${test_id}")
if(NOT test_root MATCHES "/pulp-gpu-clean-agent-preparer-[0-9a-f]+$")
    message(FATAL_ERROR "refusing unsafe preparer contract root: ${test_root}")
endif()
file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}/installed/lib")
execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DCMAKE_INSTALL_PREFIX=${test_root}/installed"
        "-DCMAKE_INSTALL_CONFIG_NAME=${BUILD_CONFIGURATION}"
        -P "${CLI_INSTALL_SCRIPT}"
    RESULT_VARIABLE install_rc
    ERROR_VARIABLE install_stderr)
if(NOT install_rc EQUAL 0)
    message(FATAL_ERROR "could not stage the installed CLI: ${install_stderr}")
endif()
file(COPY "${WEBGPU_RUNTIME_LIB}"
    DESTINATION "${test_root}/installed/lib"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
if(DEFINED V8_RUNTIME_LIBRARY AND NOT V8_RUNTIME_LIBRARY STREQUAL "")
    file(COPY "${V8_RUNTIME_LIBRARY}"
        DESTINATION "${test_root}/installed/lib"
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
endif()
set(installed_cpp_cli "${test_root}/installed/bin/pulp-cpp")
set(installed_cli "${test_root}/installed/bin/pulp")
if(NOT EXISTS "${installed_cpp_cli}")
    message(FATAL_ERROR "CLI install script did not stage pulp-cpp")
endif()
file(COPY_FILE "${installed_cpp_cli}" "${installed_cli}" ONLY_IF_DIFFERENT)
file(CHMOD "${installed_cli}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE)
set(workspace "${test_root}/workspace")
set(private_case "${test_root}/private-case")
set(plan_root "${test_root}/plan")
set(plan_document "research/a5-plan.md")
file(MAKE_DIRECTORY "${plan_root}/research")
file(WRITE "${plan_root}/${plan_document}"
    "# A5 independent clean-agent acceptance fixture\n")
execute_process(COMMAND git init -q WORKING_DIRECTORY "${plan_root}"
    RESULT_VARIABLE plan_init_rc ERROR_VARIABLE plan_init_stderr)
execute_process(COMMAND git config user.name "A5 CTest" WORKING_DIRECTORY "${plan_root}")
execute_process(COMMAND git config user.email "a5@example.invalid" WORKING_DIRECTORY "${plan_root}")
execute_process(COMMAND git add "${plan_document}" WORKING_DIRECTORY "${plan_root}")
execute_process(COMMAND git commit -qm fixture WORKING_DIRECTORY "${plan_root}"
    RESULT_VARIABLE plan_commit_rc ERROR_VARIABLE plan_commit_stderr)
execute_process(
    COMMAND git remote add origin git@github.com:danielraffel/pulp-planning.git
    WORKING_DIRECTORY "${plan_root}"
    RESULT_VARIABLE plan_remote_rc ERROR_VARIABLE plan_remote_stderr)
execute_process(
    COMMAND git update-ref refs/remotes/origin/main HEAD
    WORKING_DIRECTORY "${plan_root}"
    RESULT_VARIABLE plan_ref_rc ERROR_VARIABLE plan_ref_stderr)
if(NOT plan_init_rc EQUAL 0 OR NOT plan_commit_rc EQUAL 0 OR
   NOT plan_remote_rc EQUAL 0 OR NOT plan_ref_rc EQUAL 0)
    message(FATAL_ERROR
        "could not create isolated plan fixture: ${plan_init_stderr}${plan_commit_stderr}"
        "${plan_remote_stderr}${plan_ref_stderr}")
endif()

# This process contract deliberately stops at the nonterminal preparer state.
# A CTest process is not an independent agent and cannot certify A5 acceptance.
execute_process(
    COMMAND "${PYTHON}" "${JOURNEY_SCRIPT}" prepare
        --pulp "${installed_cli}"
        --symptom compute-readback-mismatch
        --workspace "${workspace}"
        --case-dir "${private_case}"
        --source-root "${SOURCE_ROOT}"
        --build-root "${BUILD_ROOT}"
        --cli-install-script "${CLI_INSTALL_SCRIPT}"
        --installed-prefix "${test_root}/installed"
        --plan-root "${plan_root}"
        --plan-document "${plan_document}"
    RESULT_VARIABLE prepare_rc
    OUTPUT_VARIABLE prepare_json
    ERROR_VARIABLE prepare_stderr)
if(contract_mode STREQUAL "single-config-rejection")
    if(prepare_rc EQUAL 0 OR
       NOT prepare_stderr MATCHES
           "build tree does not expose an exact Release configuration")
        message(FATAL_ERROR
            "non-Release clean-agent preparer did not fail closed (${prepare_rc}): "
            "${prepare_stderr}")
    endif()
    file(REMOVE_RECURSE "${test_root}")
    message(STATUS
        "gpu_clean_agent_preparer_nonrelease_rejected=true "
        "configuration=${BUILD_CONFIGURATION}")
    return()
endif()
pulp_gpu_clean_agent_prepare_outcome(
    prepare_outcome "${prepare_rc}" "${prepare_stderr}")
if(prepare_outcome STREQUAL "hardware-nonterminal")
    if(EXISTS "${private_case}/agent-session.json" OR
       EXISTS "${test_root}/terminal-receipt.json")
        message(FATAL_ERROR
            "preparer published terminal acceptance after a nonterminal hardware stop")
    endif()
    file(REMOVE_RECURSE "${test_root}")
    message(STATUS
        "gpu_clean_agent_preparer_hardware_nonterminal=true")
    return()
endif()
if(prepare_outcome STREQUAL "failure")
    message(FATAL_ERROR "clean-agent preparer contract failed (${prepare_rc}): ${prepare_stderr}")
endif()

string(JSON prepare_schema ERROR_VARIABLE prepare_json_error GET "${prepare_json}" schema)
string(JSON prepare_status GET "${prepare_json}" status)
string(FIND "${prepare_json}" "\"acceptance_gate_satisfied\"" prepare_gate_position)
if(prepare_json_error OR NOT prepare_schema STREQUAL "pulp.gpu-clean-agent-case.v4" OR
   NOT prepare_status STREQUAL "prepared-structural-nonterminal" OR
   NOT prepare_gate_position EQUAL -1)
    message(FATAL_ERROR "preparer did not emit the closed structural v4 boundary")
endif()
if(NOT EXISTS "${workspace}/run-probe.sh" OR
   NOT EXISTS "${private_case}/case.json" OR
   NOT EXISTS "${private_case}/reference-result.json" OR
   NOT EXISTS "${private_case}/record-signing-key.pem" OR
   NOT EXISTS "${private_case}/record-signing-public.pem" OR
   NOT EXISTS "${test_root}/installed/share/pulp/gpu-recipes.yaml" OR
   NOT EXISTS "${test_root}/installed/share/pulp/docs/guides/gpu-validation-checklist.md" OR
   NOT EXISTS "${test_root}/installed/share/pulp/docs/reference/cli.md" OR
   EXISTS "${private_case}/agent-session.json" OR
   EXISTS "${test_root}/terminal-receipt.json")
    message(FATAL_ERROR "preparer did not preserve the two-party state boundary")
endif()
file(READ "${workspace}/run-probe.sh" seeded_script LIMIT 1048576)
string(FIND "${seeded_script}" "seeded_option=\"--negative-control\"" seed_position)
if(seed_position LESS 0)
    message(FATAL_ERROR "preparer did not plant the documented incorrect option")
endif()

file(REMOVE_RECURSE "${test_root}")

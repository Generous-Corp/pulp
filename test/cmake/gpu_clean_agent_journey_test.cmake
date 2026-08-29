if(NOT DEFINED PULP_CLI OR NOT DEFINED CLI_INSTALL_SCRIPT OR
   NOT DEFINED V8_RUNTIME_LIBRARY OR NOT DEFINED WEBGPU_RUNTIME_LIB OR
   NOT DEFINED PYTHON OR NOT DEFINED JOURNEY_SCRIPT OR NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR
        "built CLI, install script, runtime libraries, Python, journey, and source are required")
endif()
if(NOT EXISTS "${PULP_CLI}" OR NOT EXISTS "${CLI_INSTALL_SCRIPT}" OR
   NOT EXISTS "${V8_RUNTIME_LIBRARY}" OR NOT EXISTS "${WEBGPU_RUNTIME_LIB}")
    message(FATAL_ERROR "built CLI installation inputs are unavailable")
endif()

execute_process(
    COMMAND git -C "${SOURCE_ROOT}" rev-parse HEAD
    RESULT_VARIABLE revision_rc
    OUTPUT_VARIABLE source_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE)
string(LENGTH "${source_revision}" source_revision_length)
if(NOT revision_rc EQUAL 0 OR NOT source_revision_length EQUAL 40 OR
   NOT source_revision MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "could not bind the preparer contract to the source revision")
endif()

get_filename_component(temp_root "$ENV{TMPDIR}" REALPATH)
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
        -P "${CLI_INSTALL_SCRIPT}"
    RESULT_VARIABLE install_rc
    ERROR_VARIABLE install_stderr)
if(NOT install_rc EQUAL 0)
    message(FATAL_ERROR "could not stage the installed CLI: ${install_stderr}")
endif()
file(COPY "${V8_RUNTIME_LIBRARY}" "${WEBGPU_RUNTIME_LIB}"
    DESTINATION "${test_root}/installed/lib"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
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

# This process contract deliberately stops at the nonterminal preparer state.
# A CTest process is not an independent agent and cannot certify A5 acceptance.
execute_process(
    COMMAND "${PYTHON}" "${JOURNEY_SCRIPT}" prepare
        --pulp "${installed_cli}"
        --symptom compute-readback-mismatch
        --workspace "${workspace}"
        --case-dir "${private_case}"
        --source-revision "${source_revision}"
        --plan-revision "${source_revision}"
        --forbidden-root "${SOURCE_ROOT}"
    RESULT_VARIABLE prepare_rc
    OUTPUT_VARIABLE prepare_json
    ERROR_VARIABLE prepare_stderr)
if(NOT prepare_rc EQUAL 0)
    message(FATAL_ERROR "clean-agent preparer contract failed (${prepare_rc}): ${prepare_stderr}")
endif()

string(JSON prepare_schema ERROR_VARIABLE prepare_json_error GET "${prepare_json}" schema)
string(JSON prepare_status GET "${prepare_json}" status)
string(JSON prepare_gate GET "${prepare_json}" acceptance_gate_satisfied)
if(prepare_json_error OR NOT prepare_schema STREQUAL "pulp.gpu-clean-agent-case.v2" OR
   NOT prepare_status STREQUAL "awaiting-independent-agent" OR prepare_gate)
    message(FATAL_ERROR "preparer incorrectly emitted terminal acceptance")
endif()
if(NOT EXISTS "${workspace}/run-probe.sh" OR
   NOT EXISTS "${private_case}/case.json" OR
   NOT EXISTS "${private_case}/reference-result.json" OR
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

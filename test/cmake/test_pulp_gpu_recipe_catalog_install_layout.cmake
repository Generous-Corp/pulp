cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR
        "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

get_filename_component(PULP_BUILD_DIR "${PULP_BUILD_DIR}" ABSOLUTE)
get_filename_component(PULP_SOURCE_DIR "${PULP_SOURCE_DIR}" ABSOLUTE)
if(NOT EXISTS "${PULP_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR
        "PULP_BUILD_DIR=${PULP_BUILD_DIR} is not a configured build")
endif()

set(_prefix
    "${CMAKE_CURRENT_BINARY_DIR}/pulp-gpu-recipe-catalog-install-test-prefix")
file(REMOVE_RECURSE "${_prefix}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        --install "${PULP_BUILD_DIR}"
        --prefix "${_prefix}"
        --component gpu-recipe-catalog
    RESULT_VARIABLE _install_rc
    OUTPUT_VARIABLE _install_stdout
    ERROR_VARIABLE _install_stderr)
if(NOT _install_rc EQUAL 0)
    message(FATAL_ERROR
        "GPU recipe catalog component install failed (${_install_rc}):\n"
        "${_install_stdout}${_install_stderr}")
endif()

foreach(_name IN ITEMS gpu-recipes.yaml gpu-recipes.schema.json)
    set(_source "${PULP_SOURCE_DIR}/docs/status/${_name}")
    set(_installed "${_prefix}/share/pulp/${_name}")
    if(NOT EXISTS "${_installed}")
        message(FATAL_ERROR
            "GPU recipe catalog component omitted ${_installed}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${_source}" "${_installed}"
        RESULT_VARIABLE _compare_rc)
    if(NOT _compare_rc EQUAL 0)
        message(FATAL_ERROR
            "Installed ${_name} differs from the selected source bytes")
    endif()
endforeach()

set(_mutated_catalog "${_prefix}/mutated-gpu-recipes.yaml")
file(COPY_FILE
    "${_prefix}/share/pulp/gpu-recipes.yaml"
    "${_mutated_catalog}")
file(APPEND "${_mutated_catalog}" "\n# seeded byte mutation\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${PULP_SOURCE_DIR}/docs/status/gpu-recipes.yaml"
        "${_mutated_catalog}"
    RESULT_VARIABLE _mutation_compare_rc)
if(_mutation_compare_rc EQUAL 0)
    message(FATAL_ERROR
        "GPU recipe catalog byte verifier accepted a seeded mutation")
endif()

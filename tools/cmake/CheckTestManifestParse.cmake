# Parse-check every CTest manifest under a directory, including the ones guarded
# by an off-by-default option.
#
# A manifest body that only runs behind an opt-in flag such as
# PULP_ENABLE_SCENE3D is never parsed by an ordinary configure, so a syntax or
# flow-control error inside it survives a fully green CI run and only surfaces
# for whoever next enables the flag. Each manifest is wrapped in `if(FALSE)`
# and run through `cmake -P`: CMake parses the whole file at load time and
# skips execution, so unbalanced parentheses and improperly nested flow control
# are reported without needing the targets, tools, or dependencies the
# registrations reference.
#
# Usage:
#   cmake -DPULP_MANIFEST_DIR=<dir> -DPULP_MANIFEST_WORK_DIR=<dir> \
#         -P tools/cmake/CheckTestManifestParse.cmake

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_MANIFEST_DIR)
    message(FATAL_ERROR "PULP_MANIFEST_DIR is required")
endif()
if(NOT IS_DIRECTORY "${PULP_MANIFEST_DIR}")
    message(FATAL_ERROR "PULP_MANIFEST_DIR is not a directory: ${PULP_MANIFEST_DIR}")
endif()
# Required rather than defaulted: the work directory is deleted and recreated on
# every run, so it must be a caller-owned scratch path.
if(NOT DEFINED PULP_MANIFEST_WORK_DIR)
    message(FATAL_ERROR "PULP_MANIFEST_WORK_DIR is required")
endif()

file(GLOB manifests "${PULP_MANIFEST_DIR}/*.cmake")
list(SORT manifests)
list(LENGTH manifests manifest_count)
if(manifest_count EQUAL 0)
    message(FATAL_ERROR
        "no *.cmake manifests found under ${PULP_MANIFEST_DIR} — the check would "
        "pass vacuously")
endif()

file(REMOVE_RECURSE "${PULP_MANIFEST_WORK_DIR}")
file(MAKE_DIRECTORY "${PULP_MANIFEST_WORK_DIR}")

set(failures "")
foreach(manifest IN LISTS manifests)
    get_filename_component(name "${manifest}" NAME)
    file(READ "${manifest}" body)
    set(wrapper "${PULP_MANIFEST_WORK_DIR}/${name}")
    file(WRITE "${wrapper}" "if(FALSE)\n${body}\nendif()\n")

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${wrapper}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE stdout_text
        ERROR_VARIABLE stderr_text)
    if(NOT status EQUAL 0)
        string(STRIP "${stderr_text}${stdout_text}" detail)
        string(REPLACE "${wrapper}" "${manifest}" detail "${detail}")
        list(APPEND failures "${name}: ${detail}")
    endif()
endforeach()

if(failures)
    foreach(failure IN LISTS failures)
        message(STATUS "manifest_parse_failed=${failure}")
    endforeach()
    list(LENGTH failures failure_count)
    message(FATAL_ERROR
        "${failure_count} of ${manifest_count} CTest manifests failed to parse")
endif()

message(STATUS "manifest_parse_checked=${manifest_count}")
message(STATUS "cmake_test_manifest_parse_verified=true")

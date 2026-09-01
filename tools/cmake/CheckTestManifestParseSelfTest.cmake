# Negative control for CheckTestManifestParse.cmake.
#
# A guard that can only report success is indistinguishable from no guard, so
# this drives the checker over deliberately malformed manifests and fails
# unless each one is rejected. The malformed shapes mirror the ways a manifest
# split has actually broken files: a command truncated before its closing
# parenthesis, an `endif()` whose `if()` lives in another file, and an `if()`
# that never closes.
#
# Usage:
#   cmake -DPULP_MANIFEST_GUARD=<path to CheckTestManifestParse.cmake> \
#         -DPULP_MANIFEST_WORK_DIR=<dir> \
#         -P tools/cmake/CheckTestManifestParseSelfTest.cmake

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_MANIFEST_GUARD)
    message(FATAL_ERROR "PULP_MANIFEST_GUARD is required")
endif()
if(NOT EXISTS "${PULP_MANIFEST_GUARD}")
    message(FATAL_ERROR "guard script not found: ${PULP_MANIFEST_GUARD}")
endif()
if(NOT DEFINED PULP_MANIFEST_WORK_DIR)
    message(FATAL_ERROR "PULP_MANIFEST_WORK_DIR is required")
endif()

set(healthy_body
"add_test(NAME selftest-healthy COMMAND selftest-tool --flag)
if(SOME_CONDITION)
    add_test(NAME selftest-healthy-guarded COMMAND selftest-tool --other)
endif()
")

# Truncated mid-command: the split that dropped a chunk's trailing lines.
set(truncated_body
"add_test(NAME selftest-truncated COMMAND selftest-tool
    --flag
")

# `endif()` whose opening `if()` was left in a sibling file.
set(stray_endif_body
"add_test(NAME selftest-stray COMMAND selftest-tool --flag)
endif()
")

# `if()` whose `endif()` was left in a sibling file.
set(unclosed_if_body
"if(SOME_CONDITION)
    add_test(NAME selftest-unclosed COMMAND selftest-tool --flag)
")

set(errors "")

function(run_guard case_name manifest_dir expect_success)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DPULP_MANIFEST_DIR=${manifest_dir}"
            "-DPULP_MANIFEST_WORK_DIR=${manifest_dir}-work"
            -P "${PULP_MANIFEST_GUARD}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    set(succeeded FALSE)
    if(status EQUAL 0)
        set(succeeded TRUE)
    endif()
    if(succeeded AND NOT expect_success)
        set(errors "${errors};${case_name}: guard accepted a malformed manifest" PARENT_SCOPE)
    elseif(NOT succeeded AND expect_success)
        set(errors "${errors};${case_name}: guard rejected a healthy manifest: ${err}${out}" PARENT_SCOPE)
    else()
        message(STATUS "manifest_parse_selftest_case=${case_name}")
    endif()
endfunction()

function(write_case case_name body)
    set(dir "${PULP_MANIFEST_WORK_DIR}/${case_name}")
    file(REMOVE_RECURSE "${dir}" "${dir}-work")
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/manifest.cmake" "${body}")
    set(case_dir "${dir}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${PULP_MANIFEST_WORK_DIR}")
file(MAKE_DIRECTORY "${PULP_MANIFEST_WORK_DIR}")

write_case(healthy "${healthy_body}")
run_guard(healthy "${case_dir}" TRUE)

write_case(truncated-command "${truncated_body}")
run_guard(truncated-command "${case_dir}" FALSE)

write_case(stray-endif "${stray_endif_body}")
run_guard(stray-endif "${case_dir}" FALSE)

write_case(unclosed-if "${unclosed_if_body}")
run_guard(unclosed-if "${case_dir}" FALSE)

# An empty directory must not read as a clean bill of health.
set(empty_dir "${PULP_MANIFEST_WORK_DIR}/empty")
file(REMOVE_RECURSE "${empty_dir}" "${empty_dir}-work")
file(MAKE_DIRECTORY "${empty_dir}")
run_guard(empty-directory-rejected "${empty_dir}" FALSE)

if(errors)
    foreach(error IN LISTS errors)
        if(error)
            message(STATUS "error=${error}")
        endif()
    endforeach()
    message(FATAL_ERROR "CheckTestManifestParse self-test failed")
endif()

message(STATUS "cmake_test_manifest_parse_selftest_verified=true")

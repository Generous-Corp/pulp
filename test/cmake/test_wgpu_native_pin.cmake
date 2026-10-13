# Script-mode test for tools/cmake/PulpWgpuNative.cmake: the wgpu-native
# runtime archive is used only when its SHA-256 matches the manifest pin.
#
#   cmake -P test/cmake/test_wgpu_native_pin.cmake
#
# Every case runs in a child `cmake -P` so a FATAL_ERROR refusal can be
# asserted. Downloads use file:// URLs into a scratch directory; nothing touches
# the network or the real source cache.

cmake_minimum_required(VERSION 3.24)

get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_module "${_repo}/tools/cmake/PulpWgpuNative.cmake")

# ── Child mode: perform one call and let FATAL_ERROR set the exit code. ──────
if(DEFINED PULP_WGPU_TEST_CASE)
    include("${_module}")
    set(PULP_WGPU_NATIVE_MANIFEST "${PULP_WGPU_TEST_MANIFEST}")
    set(CMAKE_BINARY_DIR "${PULP_WGPU_TEST_BUILD}")
    if(PULP_WGPU_TEST_DISCONNECTED)
        set(FETCHCONTENT_FULLY_DISCONNECTED ON)
    endif()
    if(PULP_WGPU_TEST_CASE STREQUAL "fetch")
        pulp_wgpu_native_fetch_verified(wgpu-test-release v1.0.0
            "${PULP_WGPU_TEST_BUILD}/verified" _dir)
        message(STATUS "RESULT_DIR=${_dir}")
    elseif(PULP_WGPU_TEST_CASE STREQUAL "select")
        set(FETCHCONTENT_SOURCE_DIR_WGPU-TEST-RELEASE "${PULP_WGPU_TEST_SOURCE}")
        set(PULP_FETCHCONTENT_SOURCE_IS_SHARED_WGPU-TEST-RELEASE "${PULP_WGPU_TEST_SHARED}")
        pulp_wgpu_native_select_source(wgpu-test-release v1.0.0 _dir)
        message(STATUS "RESULT_DIR=${_dir}")
    elseif(PULP_WGPU_TEST_CASE STREQUAL "pin")
        pulp_wgpu_native_pin(wgpu-test-release "${PULP_WGPU_TEST_VERSION}" _u _s _t)
        message(STATUS "RESULT_SHA=${_s}")
    elseif(PULP_WGPU_TEST_CASE STREQUAL "assert")
        # FetchContent's population state is project-mode only, so this stands
        # in for it with the values a populate would leave behind; the real
        # integration is the configure that calls the assert after
        # FetchContent_MakeAvailable(webgpu).
        function(FetchContent_GetProperties name)
            cmake_parse_arguments(A "" "SOURCE_DIR;POPULATED" "" ${ARGN})
            if(name STREQUAL "${PULP_WGPU_TEST_POPULATED_NAME}")
                set(${A_SOURCE_DIR} "${PULP_WGPU_TEST_POPULATED_DIR}" PARENT_SCOPE)
                set(${A_POPULATED} TRUE PARENT_SCOPE)
            else()
                set(${A_SOURCE_DIR} "" PARENT_SCOPE)
                set(${A_POPULATED} FALSE PARENT_SCOPE)
            endif()
        endfunction()
        set(PULP_WGPU_NATIVE_ASSET wgpu-test-release)
        set(PULP_WGPU_NATIVE_SOURCE_DIR "${PULP_WGPU_TEST_SOURCE}")
        pulp_wgpu_native_assert_used_verified_source()
        message(STATUS "RESULT_ASSERT=ok")
    elseif(PULP_WGPU_TEST_CASE STREQUAL "tree")
        pulp_wgpu_native_tree_sha256("${PULP_WGPU_TEST_SOURCE}" _t)
        message(STATUS "RESULT_TREE=${_t}")
    endif()
    return()
endif()

# ── Parent mode ──────────────────────────────────────────────────────────────
set(_failures 0)
# The condition is a string of if() arguments, evaluated as written.
macro(_expect cond label)
    cmake_language(EVAL CODE "
        if(${cond})
            message(STATUS \"ok   ${label}\")
        else()
            message(STATUS \"FAIL ${label}\")
            math(EXPR _failures \"\${_failures} + 1\")
        endif()")
endmacro()

set(_scratch "${CMAKE_CURRENT_BINARY_DIR}/wgpu-native-pin-test")
file(REMOVE_RECURSE "${_scratch}")
file(MAKE_DIRECTORY "${_scratch}/payload/lib" "${_scratch}/payload/include/webgpu")
file(WRITE "${_scratch}/payload/lib/a.txt" "alpha\n")
file(WRITE "${_scratch}/payload/include/webgpu/b.h" "beta\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${_scratch}/wgpu-test-release.zip" --format=zip lib include
    WORKING_DIRECTORY "${_scratch}/payload"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "fixture: could not build the test archive")
endif()
file(SHA256 "${_scratch}/wgpu-test-release.zip" _good)
set(_wrong "0000000000000000000000000000000000000000000000000000000000000000")
# The same tree hashed by tools/deps/wgpu_native_digests.py; equality proves the
# two implementations agree on the tree-digest format.
set(_good_tree "73a352a8a1a8e3460abec64e8b536c0962580c281aec2d0301e8377cf40fc1ef")

function(_write_manifest path archive_sha tree_sha version)
    file(WRITE "${path}" "{\"dependencies\": [
  {\"name\": \"other\", \"version\": \"x\"},
  {\"name\": \"wgpu-native\", \"version\": \"${version}\", \"artifacts\": [
    {\"name\": \"wgpu-test-release\", \"url\": \"file://${_scratch}/wgpu-test-release.zip\",
     \"sha256\": \"${archive_sha}\", \"tree_sha256\": \"${tree_sha}\"}]}]}\n")
endfunction()

# _run(<case> <manifest> <build> [extra -D args...]) -> _out, _code
function(_run case manifest build)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -DPULP_WGPU_TEST_CASE=${case}
            -DPULP_WGPU_TEST_MANIFEST=${manifest}
            -DPULP_WGPU_TEST_BUILD=${build}
            ${ARGN}
            -P "${CMAKE_CURRENT_LIST_FILE}"
        RESULT_VARIABLE _code
        OUTPUT_VARIABLE _o
        ERROR_VARIABLE _e)
    set(_out "${_o}${_e}" PARENT_SCOPE)
    set(_code "${_code}" PARENT_SCOPE)
endfunction()

_write_manifest("${_scratch}/good.json" "${_good}" "${_good_tree}" v1.0.0)
_write_manifest("${_scratch}/bad-archive.json" "${_wrong}" "${_good_tree}" v1.0.0)
_write_manifest("${_scratch}/bad-tree.json" "${_good}" "${_wrong}" v1.0.0)

# Tree digest agrees with the Python implementation.
_run(tree "${_scratch}/good.json" "${_scratch}/b0" "-DPULP_WGPU_TEST_SOURCE=${_scratch}/payload")
string(FIND "${_out}" "RESULT_TREE=${_good_tree}" _hit)
_expect("NOT _hit EQUAL -1" "CMake tree digest matches wgpu_native_digests.py")

# A matching archive is downloaded, verified and extracted.
_run(fetch "${_scratch}/good.json" "${_scratch}/b1")
string(FIND "${_out}" "RESULT_DIR=${_scratch}/b1/verified/wgpu-test-release-v1.0.0" _hit)
_expect("_code EQUAL 0 AND NOT _hit EQUAL -1" "the pinned archive is accepted")
_expect("EXISTS \"${_scratch}/b1/verified/wgpu-test-release-v1.0.0/lib/a.txt\""
    "control: the accepted archive was extracted")

# A wrong archive digest is refused with both digests and the fix.
_run(fetch "${_scratch}/bad-archive.json" "${_scratch}/b2")
_expect("NOT _code EQUAL 0" "a wrong archive digest fails configure")
string(FIND "${_out}" "expected: ${_wrong}" _e1)
string(FIND "${_out}" "found:    ${_good}" _e2)
string(FIND "${_out}" "tools/deps/manifest.json" _e3)
_expect("NOT _e1 EQUAL -1 AND NOT _e2 EQUAL -1 AND NOT _e3 EQUAL -1"
    "the refusal names the expected and found digests and where the pin lives")
_expect("NOT EXISTS \"${_scratch}/b2/verified/wgpu-test-release-v1.0.0\""
    "a refused archive is never extracted")
_expect("NOT EXISTS \"${_scratch}/b2/verified/wgpu-test-release-v1.0.0.zip\""
    "a refused archive is deleted")

# A wrong tree digest is refused too.
_run(fetch "${_scratch}/bad-tree.json" "${_scratch}/b3")
string(FIND "${_out}" "unexpected tree" _hit)
_expect("NOT _code EQUAL 0 AND NOT _hit EQUAL -1" "a wrong tree digest fails configure")

# FETCHCONTENT_FULLY_DISCONNECTED refuses to download.
_run(fetch "${_scratch}/good.json" "${_scratch}/b4" -DPULP_WGPU_TEST_DISCONNECTED=ON)
string(FIND "${_out}" "FETCHCONTENT_FULLY_DISCONNECTED is ON" _hit)
_expect("NOT _code EQUAL 0 AND NOT _hit EQUAL -1" "disconnected configure refuses to download")
_expect("NOT EXISTS \"${_scratch}/b4/verified/wgpu-test-release-v1.0.0\""
    "disconnected configure fetched nothing")

# A version the manifest does not pin is refused.
_run(pin "${_scratch}/good.json" "${_scratch}/b5" -DPULP_WGPU_TEST_VERSION=v9.9.9)
string(FIND "${_out}" "records v1.0.0" _hit)
_expect("NOT _code EQUAL 0 AND NOT _hit EQUAL -1" "a version drift between CMake and the manifest fails")

# A shared cache whose files match the tree digest is used offline, no download.
file(COPY "${_scratch}/payload/" DESTINATION "${_scratch}/shared/wgpu-test-release-v1.0.0")
_run(select "${_scratch}/good.json" "${_scratch}/b6" -DPULP_WGPU_TEST_DISCONNECTED=ON
    "-DPULP_WGPU_TEST_SOURCE=${_scratch}/shared/wgpu-test-release-v1.0.0" -DPULP_WGPU_TEST_SHARED=TRUE)
string(FIND "${_out}" "RESULT_DIR=${_scratch}/shared/wgpu-test-release-v1.0.0" _hit)
_expect("_code EQUAL 0 AND NOT _hit EQUAL -1" "a matching shared cache is used without downloading")

# A tampered shared cache is ignored: offline that is an error, never a use.
file(WRITE "${_scratch}/shared/wgpu-test-release-v1.0.0/lib/a.txt" "tampered\n")
_run(select "${_scratch}/good.json" "${_scratch}/b7" -DPULP_WGPU_TEST_DISCONNECTED=ON
    "-DPULP_WGPU_TEST_SOURCE=${_scratch}/shared/wgpu-test-release-v1.0.0" -DPULP_WGPU_TEST_SHARED=TRUE)
string(FIND "${_out}" "RESULT_DIR=" _used)
_expect("NOT _code EQUAL 0 AND _used EQUAL -1" "a tampered shared cache is never used")

# Online, the tampered cache is replaced by a verified build-local copy.
_run(select "${_scratch}/good.json" "${_scratch}/b8"
    "-DPULP_WGPU_TEST_SOURCE=${_scratch}/shared/wgpu-test-release-v1.0.0" -DPULP_WGPU_TEST_SHARED=TRUE)
string(FIND "${_out}" "RESULT_DIR=${_scratch}/b8/_deps/pulp-wgpu-native/wgpu-test-release-v1.0.0" _hit)
_expect("_code EQUAL 0 AND NOT _hit EQUAL -1" "online, a tampered cache falls back to a verified download")

# An explicit developer override is honoured as-is.
_run(select "${_scratch}/good.json" "${_scratch}/b9" -DPULP_WGPU_TEST_DISCONNECTED=ON
    "-DPULP_WGPU_TEST_SOURCE=${_scratch}/shared/wgpu-test-release-v1.0.0" -DPULP_WGPU_TEST_SHARED=FALSE)
string(FIND "${_out}" "explicit source override" _hit)
_expect("_code EQUAL 0 AND NOT _hit EQUAL -1" "an explicit override stays developer-managed")

# The post-populate check accepts only the verified tree WebGPU-distribution
# populated under the asset's own name.
set(_verified "${_scratch}/b1/verified/wgpu-test-release-v1.0.0")
_run(assert "${_scratch}/good.json" "${_scratch}/b10" "-DPULP_WGPU_TEST_SOURCE=${_verified}"
    -DPULP_WGPU_TEST_POPULATED_NAME=wgpu-test-release "-DPULP_WGPU_TEST_POPULATED_DIR=${_verified}/lib/..")
string(FIND "${_out}" "RESULT_ASSERT=ok" _hit)
_expect("_code EQUAL 0 AND NOT _hit EQUAL -1" "the populate check accepts the verified tree (path normalized)")
_run(assert "${_scratch}/good.json" "${_scratch}/b11" "-DPULP_WGPU_TEST_SOURCE=${_verified}"
    -DPULP_WGPU_TEST_POPULATED_NAME=wgpu-test-release "-DPULP_WGPU_TEST_POPULATED_DIR=${_scratch}/payload")
string(FIND "${_out}" "did not use the digest-verified" _hit)
_expect("NOT _code EQUAL 0 AND NOT _hit EQUAL -1" "the populate check refuses a different tree")
_run(assert "${_scratch}/good.json" "${_scratch}/b12" "-DPULP_WGPU_TEST_SOURCE=${_verified}"
    -DPULP_WGPU_TEST_POPULATED_NAME=wgpu-other-release "-DPULP_WGPU_TEST_POPULATED_DIR=${_verified}")
string(FIND "${_out}" "did not use the digest-verified" _hit)
_expect("NOT _code EQUAL 0 AND NOT _hit EQUAL -1"
    "the populate check refuses when another asset name was populated")

# A build-local copy whose files changed after extraction is re-verified, not
# reused on the strength of its stamp.
file(WRITE "${_verified}/lib/a.txt" "tampered\n")
_run(fetch "${_scratch}/good.json" "${_scratch}/b1" -DPULP_WGPU_TEST_DISCONNECTED=ON)
_expect("NOT _code EQUAL 0" "a tampered build-local copy is not reused offline")
_run(fetch "${_scratch}/good.json" "${_scratch}/b1")
_expect("_code EQUAL 0" "online, a tampered build-local copy is re-fetched")
file(READ "${_verified}/lib/a.txt" _restored)
_expect("_restored STREQUAL \"alpha\n\"" "the re-fetched build-local copy holds the verified bytes")

file(REMOVE_RECURSE "${_scratch}")
if(_failures GREATER 0)
    message(FATAL_ERROR "${_failures} wgpu-native pin check(s) failed")
endif()
message(STATUS "All wgpu-native pin checks passed")

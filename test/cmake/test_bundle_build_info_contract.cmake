# Contract for the pulp-build-info.json / runtime-pins.json machinery that does
# not need a built bundle: the build-time finalize script (commit resolution,
# overrides, embedding, invalid-JSON refusal) and the pin parsers, checked
# against oracles that are NOT the parsers' own inputs where one exists.
#
# Inputs: -DPULP_SOURCE_DIR=<repo> -DWORK_DIR=<scratch dir>

foreach(_required PULP_SOURCE_DIR WORK_DIR)
    if(NOT DEFINED ${_required})
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

set(_finalize "${PULP_SOURCE_DIR}/tools/cmake/PulpFinalizeBuildInfo.cmake")
include("${PULP_SOURCE_DIR}/tools/cmake/PulpRuntimePins.cmake")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(_expect label actual expected)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: expected '${expected}', got '${actual}'")
    endif()
    message(STATUS "ok: ${label} = ${actual}")
endfunction()

function(_finalize out_json)
    cmake_parse_arguments(F "NO_GIT_HINT" "GIT_DIR;GIT_SHA;GIT_DIRTY;EMBED;EXPECT_FAIL" "" ${ARGN})
    set(_out "${WORK_DIR}/out.json")
    file(REMOVE "${_out}")
    find_program(_git git)
    if(F_NO_GIT_HINT)
        set(_git "")
    endif()
    # The scratch dir usually sits inside a build tree inside a checkout; the
    # ceiling keeps git from resolving that enclosing repository.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "GIT_CEILING_DIRECTORIES=${WORK_DIR}"
            "${CMAKE_COMMAND}"
            "-DPULP_BUILD_INFO_TEMPLATE=${WORK_DIR}/template.json"
            "-DPULP_BUILD_INFO_OUTPUT=${_out}"
            "-DPULP_BUILD_INFO_GIT_DIR=${F_GIT_DIR}"
            "-DPULP_BUILD_INFO_GIT_SHA=${F_GIT_SHA}"
            "-DPULP_BUILD_INFO_GIT_DIRTY=${F_GIT_DIRTY}"
            "-DPULP_BUILD_INFO_EMBED=${F_EMBED}"
            "-DPULP_BUILD_INFO_GIT=${_git}"
            -P "${_finalize}"
        RESULT_VARIABLE _result
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    if(F_EXPECT_FAIL)
        if(_result EQUAL 0)
            message(FATAL_ERROR "finalize unexpectedly succeeded")
        endif()
        set(${out_json} "" PARENT_SCOPE)
        return()
    endif()
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "finalize failed (${_result}): ${_stdout}${_stderr}")
    endif()
    file(READ "${_out}" _json)
    set(${out_json} "${_json}" PARENT_SCOPE)
endfunction()

file(WRITE "${WORK_DIR}/template.json"
"{
  \"schema\": \"pulp.build-info.v1\",
  \"product\": {\"source_git_sha\": \"__PULP_GIT_SHA__\", \"source_git_dirty\": __PULP_GIT_DIRTY__},
  \"runtime_pins\": __PULP_EMBED__
}
")

# 1. Explicit overrides win and need no git.
_finalize(_json GIT_SHA "0123abcd" GIT_DIRTY TRUE)
string(JSON _sha GET "${_json}" product source_git_sha)
string(JSON _dirty GET "${_json}" product source_git_dirty)
string(JSON _pins_type TYPE "${_json}" runtime_pins)
_expect("override sha" "${_sha}" "0123abcd")
_expect("override dirty" "${_dirty}" "ON")
_expect("missing embed is null" "${_pins_type}" "NULL")

# 2. No git checkout and no override: "unknown" / null, never a guess.
file(MAKE_DIRECTORY "${WORK_DIR}/no-git")
_finalize(_json GIT_DIR "${WORK_DIR}/no-git")
string(JSON _sha GET "${_json}" product source_git_sha)
string(JSON _dirty_type TYPE "${_json}" product source_git_dirty)
_expect("no-git sha" "${_sha}" "unknown")
_expect("no-git dirty" "${_dirty_type}" "NULL")

# 3. Git resolution reads the checkout's HEAD — oracle is git itself.
find_program(_git git)
if(_git AND EXISTS "${PULP_SOURCE_DIR}/.git")
    execute_process(COMMAND "${_git}" -C "${PULP_SOURCE_DIR}" rev-parse HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE)
    _finalize(_json GIT_DIR "${PULP_SOURCE_DIR}/tools/cmake")
    string(JSON _sha GET "${_json}" product source_git_sha)
    string(JSON _dirty_type TYPE "${_json}" product source_git_dirty)
    _expect("git sha from a subdirectory" "${_sha}" "${_head}")
    _expect("git dirty is a boolean" "${_dirty_type}" "BOOLEAN")
    # A consumer configured without a Git package passes no executable; the
    # script must still find git itself.
    _finalize(_json GIT_DIR "${PULP_SOURCE_DIR}" NO_GIT_HINT)
    string(JSON _sha GET "${_json}" product source_git_sha)
    _expect("git sha without an executable hint" "${_sha}" "${_head}")
else()
    message(STATUS "skip: git checkout unavailable for HEAD resolution")
endif()

# 4. An embed file becomes a nested object.
file(WRITE "${WORK_DIR}/pins.json" "{\n  \"schema\": \"pulp.runtime-pins.v1\",\n  \"webgpu\": {\"wgpu_native_version\": \"v1.2.3\"}\n}\n")
_finalize(_json GIT_SHA "abc" GIT_DIRTY FALSE EMBED "${WORK_DIR}/pins.json")
string(JSON _embedded GET "${_json}" runtime_pins webgpu wgpu_native_version)
string(JSON _dirty GET "${_json}" product source_git_dirty)
_expect("embedded pin" "${_embedded}" "v1.2.3")
_expect("override clean" "${_dirty}" "OFF")

# 5. A malformed embed or an unsafe commit string is refused, not emitted.
file(WRITE "${WORK_DIR}/bad.json" "[1, 2]")
_finalize(_json GIT_SHA "abc" EMBED "${WORK_DIR}/bad.json" EXPECT_FAIL TRUE)
_finalize(_json GIT_SHA "a\"b" EXPECT_FAIL TRUE)

# 6. kDawnVersion decoding, against a synthetic header with a known SHA-1.
file(WRITE "${WORK_DIR}/dawn_version.h"
"static constexpr std::array<uint8_t, 20> kDawnVersion = { 0xf9, 0x1d, 0xa7, 0x5a, 0xfe, 0x31, 0xd4, 0xd6, 0xf4, 0x7a, 0x6d, 0xa3, 0x07, 0xe1, 0xfb, 0xab, 0xd1, 0xb1, 0x69, 0x1A };\n")
_pulp_runtime_pins_dawn_commit("${WORK_DIR}/dawn_version.h" _dawn)
_expect("dawn commit" "${_dawn}" "f91da75afe31d4d6f47a6da307e1fbabd1b1691a")
file(WRITE "${WORK_DIR}/short.h" "kDawnVersion = { 0x01, 0x02 };\n")
_pulp_runtime_pins_dawn_commit("${WORK_DIR}/short.h" _dawn)
_expect("truncated dawn version is empty" "${_dawn}" "")

# 7. The checked-in Skia VERSION.md parses to the release tools/deps/manifest.json
#    pins, and to the branch-tip commit the manifest's Dawn entry names — two
#    independent records of the same prebuilt.
_pulp_runtime_pins_skia("${PULP_SOURCE_DIR}/external/skia-build/VERSION.md"
    _release _commit _builder_ref)
file(READ "${PULP_SOURCE_DIR}/tools/deps/manifest.json" _manifest)
string(JSON _dep_count LENGTH "${_manifest}" dependencies)
math(EXPR _last "${_dep_count} - 1")
set(_manifest_skia "")
set(_manifest_dawn_notes "")
foreach(_i RANGE ${_last})
    string(JSON _name GET "${_manifest}" dependencies ${_i} name)
    if(_name STREQUAL "Skia")
        string(JSON _manifest_skia GET "${_manifest}" dependencies ${_i} version)
    elseif(_name STREQUAL "Dawn")
        string(JSON _manifest_dawn_notes GET "${_manifest}" dependencies ${_i} notes)
    endif()
endforeach()
_expect("skia release vs manifest" "${_release}" "${_manifest_skia}")
if(NOT _manifest_dawn_notes MATCHES "DEPS file at ([0-9a-f]+)")
    message(FATAL_ERROR "manifest Dawn notes no longer name the Skia DEPS commit")
endif()
_expect("skia commit vs manifest" "${_commit}" "${CMAKE_MATCH_1}")
string(LENGTH "${_builder_ref}" _builder_ref_length)
_expect("skia-builder ref length" "${_builder_ref_length}" "40")

# 8. min_os rendering carries every platform floor from min_os.json.
_pulp_runtime_pins_min_os("${PULP_SOURCE_DIR}/tools/deps/min_os.json" _min_os)
file(READ "${PULP_SOURCE_DIR}/tools/deps/min_os.json" _min_os_source)
string(JSON _expected_mac GET "${_min_os_source}" platforms macos-arm64 floor)
string(JSON _rendered_mac GET "${_min_os}" macos-arm64 floor)
_expect("macos-arm64 floor" "${_rendered_mac}" "${_expected_mac}")
string(JSON _rendered_linux_arm_type TYPE "${_min_os}" linux-arm64 floor)
_expect("unmeasured floor stays null" "${_rendered_linux_arm_type}" "NULL")

message(STATUS "bundle_build_info_contract_verified=true")

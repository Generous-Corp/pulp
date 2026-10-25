#[[
Sealed V8 provider consumer_defines contract.

Each case lays out a provider root that passes FindV8's generation-receipt check
against the real pins in tools/deps/manifest.json, varies only its manifest.json,
and configures a throwaway LANGUAGES NONE project that includes the real
FindV8.cmake. The positive case reads v8::v8's INTERFACE_COMPILE_DEFINITIONS back;
the negative cases require configure to fail with the module's diagnostic. The
legacy V8_INCLUDE_DIR/V8_LIB_DIR override has no manifest and must stay
define-free, and the Windows ABI helper must add V8_COMPRESS_POINTERS only when
v8::v8 does not already carry it.

Run with: cmake -DPULP_TEST_GENERATOR=<generator> -P test_v8_consumer_defines.cmake
]]

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_work "${CMAKE_CURRENT_BINARY_DIR}/v8-consumer-defines-probes")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}")

file(READ "${_repo_root}/CMakeLists.txt" _root_cmakelists)
if(NOT _root_cmakelists MATCHES "cmake_minimum_required\\(VERSION ([0-9]+\\.[0-9]+)")
    message(FATAL_ERROR "could not read the policy floor from ${_repo_root}/CMakeLists.txt")
endif()
set(_policy_floor "${CMAKE_MATCH_1}")
unset(_root_cmakelists)

set(_generator_args "")
if(PULP_TEST_GENERATOR)
    set(_generator_args -G "${PULP_TEST_GENERATOR}")
endif()

# ── Pins the fixture provider must satisfy ───────────────────────────────────
file(READ "${_repo_root}/tools/deps/manifest.json" _deps_json)
string(JSON _dep_count LENGTH "${_deps_json}" dependencies)
math(EXPR _dep_last "${_dep_count} - 1")
set(_v8_index "")
foreach(_i RANGE 0 ${_dep_last})
    string(JSON _name GET "${_deps_json}" dependencies ${_i} name)
    if(_name STREQUAL "V8")
        set(_v8_index ${_i})
        break()
    endif()
endforeach()
if(_v8_index STREQUAL "")
    message(FATAL_ERROR "tools/deps/manifest.json has no V8 dependency entry")
endif()
string(JSON _metadata_sha ERROR_VARIABLE _err GET "${_deps_json}"
    dependencies ${_v8_index} determinism release_metadata_sha256)
if(NOT _err STREQUAL "NOTFOUND")
    set(_metadata_sha "")
endif()

# Every platform key FindV8 can select on a desktop host; the fixture provides
# all of them so the probe resolves whichever key this host maps to.
set(_keys mac-arm64 mac-x86_64 linux-x64 linux-arm64 win-x64 win-arm64)
set(_stub_files include/v8.h lib/libv8.dylib lib/libv8.so lib/v8.dll lib/v8.dll.lib)

# Write a receipt-valid provider for every key under base. manifest_text is the
# provider's manifest.json body, or the literal ABSENT to omit the file.
function(_write_provider base manifest_text)
    foreach(_key IN LISTS _keys)
        set(_root "${base}/${_key}")
        string(JSON _asset_sha GET "${_deps_json}"
            dependencies ${_v8_index} determinism release_assets ${_key} sha256)
        set(_files ${_stub_files})
        foreach(_rel IN LISTS _stub_files)
            file(WRITE "${_root}/${_rel}" "fixture ${_rel}\n")
        endforeach()
        if(NOT manifest_text STREQUAL "ABSENT")
            file(WRITE "${_root}/manifest.json" "${manifest_text}")
            list(APPEND _files manifest.json)
        endif()
        set(_entries "")
        foreach(_rel IN LISTS _files)
            file(SIZE "${_root}/${_rel}" _size)
            file(SHA256 "${_root}/${_rel}" _sha)
            list(APPEND _entries
                "{\"path\": \"${_rel}\", \"size\": ${_size}, \"sha256\": \"${_sha}\"}")
        endforeach()
        list(JOIN _entries ", " _entries)
        file(WRITE "${_root}/.v8-asset-sha256" "${_asset_sha}\n")
        file(WRITE "${_root}/.v8-release-metadata-sha256" "${_metadata_sha}\n")
        file(WRITE "${_root}/.v8-generation-manifest.json"
            "{\"schema\": 1, \"platform\": \"${_key}\", "
            "\"asset_sha256\": \"${_asset_sha}\", "
            "\"release_metadata_sha256\": \"${_metadata_sha}\", "
            "\"files\": [${_entries}]}\n")
    endforeach()
endfunction()

set(_probe_project [=[
cmake_minimum_required(VERSION @_policy_floor@)
project(pulp_v8_consumer_defines_probe LANGUAGES NONE)
set(PULP_ROOT_DIR "@_repo_root@")
include("@_repo_root@/tools/cmake/FindV8.cmake")
include("@_repo_root@/tools/cmake/PulpV8Windows.cmake")
if(NOT PULP_V8_FOUND)
    message(FATAL_ERROR "probe: FindV8 resolved no provider")
endif()
get_target_property(_defs v8::v8 INTERFACE_COMPILE_DEFINITIONS)
pulp_v8_windows_abi_defines(_win)
file(WRITE "${CMAKE_BINARY_DIR}/probe-result.txt"
    "defines=${_defs}\nwindows=${_win}\n")
]=])
string(CONFIGURE "${_probe_project}" _probe_project @ONLY)

# Configure the probe project. Sets <name>_status, <name>_output, <name>_result.
function(_configure name)
    set(_src "${_work}/${name}/src")
    set(_bin "${_work}/${name}/bin")
    file(WRITE "${_src}/CMakeLists.txt" "${_probe_project}")
    # A baked V8_DIR in the environment is searched before the cache value;
    # clear it so the fixture is the provider under test.
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env --unset=V8_DIR
            "${CMAKE_COMMAND}" ${_generator_args} -S "${_src}" -B "${_bin}" ${ARGN}
        RESULT_VARIABLE _status
        OUTPUT_VARIABLE _stdout
        ERROR_VARIABLE _stderr)
    set(_result "")
    if(EXISTS "${_bin}/probe-result.txt")
        file(READ "${_bin}/probe-result.txt" _result)
    endif()
    set(${name}_status "${_status}" PARENT_SCOPE)
    set(${name}_output "${_stdout}\n${_stderr}" PARENT_SCOPE)
    set(${name}_result "${_result}" PARENT_SCOPE)
endfunction()

function(_expect_sealed_failure name manifest_text expected_error)
    _write_provider("${_work}/${name}/provider" "${manifest_text}")
    _configure(${name} "-DV8_DIR=${_work}/${name}/provider")
    if(${name}_status EQUAL 0)
        message(FATAL_ERROR
            "${name}: configure accepted a sealed provider whose manifest should "
            "fail closed:\n${${name}_output}")
    endif()
    # CMake word-wraps FATAL_ERROR text; compare against single-spaced output.
    string(REGEX REPLACE "[ \t\r\n]+" " " _flat "${${name}_output}")
    if(NOT _flat MATCHES "${expected_error}")
        message(FATAL_ERROR
            "${name}: wrong diagnostic, expected '${expected_error}':\n${${name}_output}")
    endif()
    message(STATUS "${name}: configure failed closed as expected")
endfunction()

# ── Positive: every declared define lands on v8::v8, in order ────────────────
set(_declared
    V8_ARRAY_BUFFER_INTERNAL_FIELD_COUNT=0
    V8_PROMISE_INTERNAL_FIELD_COUNT=0
    V8_USE_DEFAULT_HASHER_SECRET=true
    CPPGC_CAGED_HEAP
    CPPGC_POINTER_COMPRESSION
    V8_COMPRESS_POINTERS)
list(JOIN _declared "\", \"" _declared_json)
_write_provider("${_work}/sealed/provider"
    "{\"sealed\": true, \"consumer_defines\": [\"${_declared_json}\"]}\n")
_configure(sealed "-DV8_DIR=${_work}/sealed/provider")
if(NOT sealed_status EQUAL 0)
    message(FATAL_ERROR "sealed: configure failed:\n${sealed_output}")
endif()
if(NOT sealed_output MATCHES "Pulp V8 provider: ${_work}/sealed/provider/")
    message(FATAL_ERROR
        "sealed: FindV8 did not resolve the fixture provider:\n${sealed_output}")
endif()
list(JOIN _declared ";" _declared_list)
if(NOT sealed_result STREQUAL "defines=${_declared_list}\nwindows=\n")
    message(FATAL_ERROR
        "sealed: v8::v8 does not carry exactly the declared consumer_defines, or "
        "the Windows helper duplicated V8_COMPRESS_POINTERS. Got:\n${sealed_result}")
endif()
message(STATUS "sealed: v8::v8 carries ${_declared_list}")

# ── Legacy override: no manifest, no defines, Windows adds its own ───────────
_write_provider("${_work}/legacy/provider" ABSENT)
list(GET _keys 0 _legacy_key)
_configure(legacy
    "-DV8_INCLUDE_DIR=${_work}/legacy/provider/${_legacy_key}/include"
    "-DV8_LIB_DIR=${_work}/legacy/provider/${_legacy_key}/lib")
if(NOT legacy_status EQUAL 0)
    message(FATAL_ERROR "legacy: configure failed:\n${legacy_output}")
endif()
if(NOT legacy_result STREQUAL "defines=_defs-NOTFOUND\nwindows=V8_COMPRESS_POINTERS\n")
    message(FATAL_ERROR
        "legacy: the manifest-less override must carry no consumer defines and "
        "must leave V8_COMPRESS_POINTERS to the Windows helper. Got:\n${legacy_result}")
endif()
message(STATUS "legacy: override carries no consumer defines")

# ── Negative: a sealed provider whose manifest cannot supply the ABI ─────────
_expect_sealed_failure(missing-manifest ABSENT
    "sealed V8 provider manifest .*manifest.json' does not exist")
_expect_sealed_failure(invalid-json
    "{\"sealed\": true, \"consumer_defines\": [\"CPPGC_CAGED_HEAP\""
    "is malformed")
_expect_sealed_failure(no-defines-field
    "{\"sealed\": true}\n"
    "is malformed \\(no 'consumer_defines'\\)")
_expect_sealed_failure(defines-not-array
    "{\"sealed\": true, \"consumer_defines\": \"CPPGC_CAGED_HEAP\"}\n"
    "'consumer_defines' must be an array")
_expect_sealed_failure(empty-defines
    "{\"sealed\": true, \"consumer_defines\": []}\n"
    "declares no consumer_defines")
_expect_sealed_failure(injected-flag
    "{\"sealed\": true, \"consumer_defines\": [\"CPPGC_CAGED_HEAP\", \"A=1 -DB\"]}\n"
    "consumer_defines\\[1\\] is not a NAME or NAME=VALUE token")
_expect_sealed_failure(list-separator
    "{\"sealed\": true, \"consumer_defines\": [\"A=1;B\"]}\n"
    "consumer_defines\\[0\\] is not a NAME or NAME=VALUE token")
_expect_sealed_failure(non-string-define
    "{\"sealed\": true, \"consumer_defines\": [1]}\n"
    "consumer_defines\\[0\\] is not a NAME or NAME=VALUE token")
_expect_sealed_failure(not-sealed
    "{\"sealed\": false, \"consumer_defines\": [\"CPPGC_CAGED_HEAP\"]}\n"
    "does not declare sealed=true")

file(REMOVE_RECURSE "${_work}")

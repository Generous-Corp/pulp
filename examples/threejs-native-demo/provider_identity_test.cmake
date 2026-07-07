# Strict V8 provider-identity + cube-coexistence CTest for pulp #30.
#
# Unlike capture_test.cmake (which tolerates "no V8 / no Dawn" as a pass to
# guard only against the #542 hang), this test has NO skip-pass: it asserts the
# sealed v8-builder seal is actually the linked runtime AND that V8 coexists
# with Dawn/Skia in the real app by producing a non-empty cube PNG. It is gated
# on PULP_VALIDATE_V8_PROVIDER_STRICT and only added when the v8-builder
# provider is the configured V8 source.
#
# Inputs (set via -D):
#   DEMO_BIN          : path to the built pulp-threejs-native-demo binary
#   CAPTURE_PATH      : path where the cube demo should write the PNG
#   EXPECTED_VERSION  : expected V8 runtime version (e.g. 15.2.24); optional
#   EXPECTED_KIND     : expected provider kind (default: v8builder)

if(NOT DEFINED DEMO_BIN)
    message(FATAL_ERROR "provider_identity_test.cmake: DEMO_BIN not set")
endif()
if(NOT DEFINED CAPTURE_PATH)
    message(FATAL_ERROR "provider_identity_test.cmake: CAPTURE_PATH not set")
endif()
if(NOT DEFINED EXPECTED_KIND)
    set(EXPECTED_KIND "v8builder")
endif()

# ── 1. Engine identity ──────────────────────────────────────────────────────
execute_process(
    COMMAND "${DEMO_BIN}" --print-engine-identity
    TIMEOUT 30
    RESULT_VARIABLE id_result
    OUTPUT_VARIABLE id_stdout
    ERROR_VARIABLE id_stderr
)

message(STATUS "engine-identity stdout:\n${id_stdout}")
if(id_stderr)
    message(STATUS "engine-identity stderr:\n${id_stderr}")
endif()

if(NOT id_result EQUAL 0)
    message(FATAL_ERROR "--print-engine-identity exited ${id_result} (stderr: ${id_stderr})")
endif()

# Parse the identity block into a flat key=value list.
if(NOT id_stdout MATCHES "PULP_ENGINE_IDENTITY_BEGIN(.*)PULP_ENGINE_IDENTITY_END")
    message(FATAL_ERROR "Identity block not found in --print-engine-identity output")
endif()
set(_id_body "${CMAKE_MATCH_1}")

function(_id_field key out_var)
    if(_id_body MATCHES "${key}=([^\n]*)")
        set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

_id_field("engine_type" engine_type)
_id_field("runtime_version" runtime_version)
_id_field("provider_kind" provider_kind)
_id_field("provider_path" provider_path)
_id_field("expected_runtime_version" expected_runtime_version)
_id_field("pulp_has_v8" pulp_has_v8)
_id_field("gpu_available" gpu_available)
_id_field("gpu_native_bridge" gpu_native_bridge)
_id_field("gpu_backend" gpu_backend)
_id_field("gpu_software" gpu_software)

# Strip CMake-list / whitespace artifacts.
string(STRIP "${engine_type}" engine_type)
string(STRIP "${runtime_version}" runtime_version)
string(STRIP "${provider_kind}" provider_kind)
string(STRIP "${pulp_has_v8}" pulp_has_v8)
string(STRIP "${gpu_software}" gpu_software)

# No skip-pass: V8 must be linked and report a real version.
if(NOT pulp_has_v8 STREQUAL "1")
    message(FATAL_ERROR "pulp_has_v8 != 1 (got '${pulp_has_v8}') — V8 not linked")
endif()
if(NOT engine_type STREQUAL "V8")
    message(FATAL_ERROR "engine_type != V8 (got '${engine_type}')")
endif()
if(runtime_version STREQUAL "")
    message(FATAL_ERROR "runtime_version is empty — V8 did not report a version")
endif()
if(NOT provider_kind STREQUAL EXPECTED_KIND)
    message(FATAL_ERROR
        "provider_kind '${provider_kind}' != expected '${EXPECTED_KIND}'")
endif()

# When an expected version is pinned, the runtime V8 reports must start with it.
if(DEFINED EXPECTED_VERSION AND NOT EXPECTED_VERSION STREQUAL "")
    if(NOT runtime_version MATCHES "^${EXPECTED_VERSION}")
        message(FATAL_ERROR
            "runtime_version '${runtime_version}' does not start with expected "
            "'${EXPECTED_VERSION}'")
    endif()
    if(NOT expected_runtime_version STREQUAL EXPECTED_VERSION)
        message(FATAL_ERROR
            "expected_runtime_version '${expected_runtime_version}' != "
            "'${EXPECTED_VERSION}' — provider manifest/CMake disagree")
    endif()
endif()

message(STATUS
    "provider identity OK: engine=${engine_type} runtime=${runtime_version} "
    "kind=${provider_kind} gpu_backend=${gpu_backend} gpu_software=${gpu_software}")

# ── 2. Cube coexistence: V8 + Dawn/Skia render a real PNG (no skip-pass) ─────
if(EXISTS "${CAPTURE_PATH}")
    file(REMOVE "${CAPTURE_PATH}")
endif()

execute_process(
    COMMAND "${DEMO_BIN}" --demo cube --capture "${CAPTURE_PATH}"
    TIMEOUT 60
    RESULT_VARIABLE cap_result
    OUTPUT_VARIABLE cap_stdout
    ERROR_VARIABLE cap_stderr
)

message(STATUS "cube-capture stdout:\n${cap_stdout}")
if(cap_stderr)
    message(STATUS "cube-capture stderr:\n${cap_stderr}")
endif()

if(NOT cap_result MATCHES "^-?[0-9]+$")
    message(FATAL_ERROR
        "cube --capture did not exit within the bounded timeout (result='${cap_result}')")
endif()
if(NOT cap_result EQUAL 0)
    message(FATAL_ERROR "cube --capture exited ${cap_result} (stderr: ${cap_stderr})")
endif()
if(NOT EXISTS "${CAPTURE_PATH}")
    message(FATAL_ERROR "Cube capture PNG not written: ${CAPTURE_PATH}")
endif()

file(SIZE "${CAPTURE_PATH}" capture_size)
if(capture_size LESS 1024)
    message(FATAL_ERROR
        "Cube capture PNG is suspiciously small (${capture_size} bytes): ${CAPTURE_PATH}")
endif()

message(STATUS
    "provider_identity_test: OK — V8 ${runtime_version} (${provider_kind}) coexists "
    "with Dawn/Skia, cube PNG ${capture_size} bytes")

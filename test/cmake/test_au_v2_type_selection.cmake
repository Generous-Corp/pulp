#[[
Pure-logic smoke test for shared AU metadata resolution. Run under
`cmake -P` so it doesn't need a real Pulp configure — that full tree is too
heavy to spin up inside ctest for metadata branch coverage.

This includes the real helper from tools/cmake/PulpPluginMetadata.cmake. If AU
metadata behavior changes, the helper changes and this test stays the
specification for which component type each (category, accepts_midi) pair emits.

Expected mapping:
  (Instrument,  *)     -> aumu
  (MidiEffect,  *)     -> aumi
  (Effect,      true)  -> aumf   <-- the fix
  (Effect,      false) -> aufx

Also pins the public CMake-option contract: `ACCEPTS_MIDI` is the only
pulp_add_plugin MIDI packaging option. MIDI output remains
PluginDescriptor/format metadata, not a parallel `PRODUCES_MIDI` CMake flag.
]]

get_filename_component(_repo_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(_metadata "${_repo_root}/tools/cmake/PulpPluginMetadata.cmake")
set(_utils "${_repo_root}/tools/cmake/PulpUtils.cmake")
set(_reference "${_repo_root}/docs/reference/cmake.md")
set(_status "${_repo_root}/docs/status/cmake-functions.yaml")

foreach(_path IN LISTS _metadata _utils _reference _status)
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "Required file missing: ${_path}")
    endif()
endforeach()

include("${_metadata}")

set(_fail 0)

function(_expect_au_meta category accepts_midi expected_type expected_tag expected_version_int)
    _pulp_metadata_resolve_au(_resolved
        "test(${category},${accepts_midi})"
        "${category}" "${accepts_midi}" "1.2.3"
        "PGan" "Pulp")
    if(NOT _resolved_TYPE STREQUAL "${expected_type}")
        message("FAIL: ${category} + accepts_midi=${accepts_midi} -> expected ${expected_type}, got ${_resolved_TYPE}")
        set(_fail 1 PARENT_SCOPE)
    endif()
    if(NOT _resolved_TAG STREQUAL "${expected_tag}")
        message("FAIL: ${category} + accepts_midi=${accepts_midi} -> expected tag ${expected_tag}, got ${_resolved_TAG}")
        set(_fail 1 PARENT_SCOPE)
    endif()
    if(NOT _resolved_VERSION_INT STREQUAL "${expected_version_int}")
        message("FAIL: ${category} + accepts_midi=${accepts_midi} -> expected AU version ${expected_version_int}, got ${_resolved_VERSION_INT}")
        set(_fail 1 PARENT_SCOPE)
    endif()
endfunction()

_expect_au_meta("Instrument" 1 "aumu" "Synthesizer" 66051)
_expect_au_meta("Instrument" 0 "aumu" "Synthesizer" 66051)
_expect_au_meta("MidiEffect" 0 "aumi" "MIDI" 66051)
_expect_au_meta("Effect" 1 "aumf" "Effects" 66051)
_expect_au_meta("Effect" 0 "aufx" "Effects" 66051)
_expect_au_meta("Effect" "" "aufx" "Effects" 66051)

_pulp_metadata_resolve_au(_resolved_suffix
    "test(version-suffix)"
    "Effect" 0 "1.2.3-beta.1+local"
    "PGan" "Pulp")
if(NOT _resolved_suffix_VERSION_INT STREQUAL "66051")
    message("FAIL: semantic version suffix should preserve numeric AU version, got ${_resolved_suffix_VERSION_INT}")
    set(_fail 1)
endif()

function(_expect_metadata_failure test_name script_body expected_error)
    if(DEFINED ENV{TMPDIR})
        set(_pulp_tmp_dir "$ENV{TMPDIR}")
    else()
        set(_pulp_tmp_dir "/tmp")
    endif()
    set(_script "${_pulp_tmp_dir}/pulp-metadata-${test_name}.cmake")
    file(WRITE "${_script}" "${script_body}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -P "${_script}"
        RESULT_VARIABLE _rv
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err)
    if(_rv EQUAL 0)
        message("FAIL: ${test_name} unexpectedly passed")
        set(_fail 1 PARENT_SCOPE)
    endif()
    if(NOT _err MATCHES "${expected_error}")
        message("FAIL: ${test_name} error was not deterministic: ${_err}")
        set(_fail 1 PARENT_SCOPE)
    endif()
endfunction()

_expect_metadata_failure(
    "bad-fourcc"
    "include(\"${_metadata}\")\n_pulp_metadata_resolve_au(_bad \"bad-fourcc\" \"Effect\" 0 \"1.0.0\" \"BAD\" \"Pulp\")\n"
    "PLUGIN_CODE/SUBTYPE_CODE must be exactly 4 characters")

_expect_metadata_failure(
    "bad-category"
    "include(\"${_metadata}\")\n_pulp_metadata_resolve_au(_bad \"bad-category\" \"Generator\" 0 \"1.0.0\" \"PGan\" \"Pulp\")\n"
    "CATEGORY must be Effect, Instrument, or MidiEffect")

_expect_metadata_failure(
    "bad-version"
    "include(\"${_metadata}\")\n_pulp_metadata_resolve_au(_bad \"bad-version\" \"Effect\" 0 \"1foo\" \"PGan\" \"Pulp\")\n"
    "VERSION must be numeric major")

if(_fail)
    message(FATAL_ERROR "AU metadata resolver smoke failed.")
endif()

file(READ "${_utils}" _utils_text)
if(NOT _utils_text MATCHES "PulpPluginMetadata\\.cmake")
    message(FATAL_ERROR "PulpUtils.cmake must include PulpPluginMetadata.cmake")
endif()

file(READ "${_metadata}" _metadata_text)
foreach(_reload_marker IN ITEMS
        "CONTENT_HOT_RELOAD_KINDS"
        "_pulp_configure_plugin_runtime_manifest"
        "_pulp_attach_plugin_runtime_manifest"
        "pulp_add_reload_logic"
        "pulp_reload_host")
    if(_metadata_text MATCHES "${_reload_marker}")
        message(FATAL_ERROR
            "PulpPluginMetadata.cmake must stay packaging-metadata-only; "
            "unexpected reload marker found: ${_reload_marker}")
    endif()
endforeach()
foreach(_utils_reload_marker IN ITEMS
        "CONTENT_HOT_RELOAD_KINDS"
        "CONTENT_MANUAL_RESCAN_KINDS"
        "hotReloadKinds"
        "manualRescanKinds"
        "function\\(pulp_add_reload_logic"
        "function\\(pulp_reload_host"
        "function\\(pulp_reload_host_ui")
    if(NOT _utils_text MATCHES "${_utils_reload_marker}")
        message(FATAL_ERROR
            "PulpUtils.cmake must preserve hot-reload/reload-host surface: "
            "missing ${_utils_reload_marker}")
    endif()
endforeach()

string(REGEX MATCH "cmake_parse_arguments\\(PLUGIN[ \t\r\n]+\"([^\"]*)\"" _parse_match "${_utils_text}")
if(NOT _parse_match)
    message(FATAL_ERROR "Could not find pulp_add_plugin cmake_parse_arguments option list")
endif()

set(_plugin_options "${CMAKE_MATCH_1}")
if(NOT _plugin_options MATCHES "(^|;)ACCEPTS_MIDI($|;)")
    message(FATAL_ERROR "pulp_add_plugin must parse ACCEPTS_MIDI")
endif()
if(_plugin_options MATCHES "(^|;)PRODUCES_MIDI($|;)")
    message(FATAL_ERROR "pulp_add_plugin must not parse inert PRODUCES_MIDI")
endif()
if(NOT _utils_text MATCHES "PRODUCES_MIDI is ignored")
    message(FATAL_ERROR "pulp_add_plugin must warn when callers pass inert PRODUCES_MIDI")
endif()

file(READ "${_reference}" _reference_text)
if(NOT _reference_text MATCHES "`ACCEPTS_MIDI`")
    message(FATAL_ERROR "docs/reference/cmake.md must document ACCEPTS_MIDI")
endif()
if(_reference_text MATCHES "\\|[ \t]*`PRODUCES_MIDI`")
    message(FATAL_ERROR "docs/reference/cmake.md must not document PRODUCES_MIDI as a CMake option")
endif()

file(READ "${_status}" _status_text)
if(NOT _status_text MATCHES "[ \t-]ACCEPTS_MIDI")
    message(FATAL_ERROR "docs/status/cmake-functions.yaml must list ACCEPTS_MIDI")
endif()
if(_status_text MATCHES "[ \t-]PRODUCES_MIDI")
    message(FATAL_ERROR "docs/status/cmake-functions.yaml must not list PRODUCES_MIDI")
endif()

message(STATUS "AU metadata resolver and pulp_add_plugin MIDI-option contract: OK")

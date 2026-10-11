# pulp_add_plugin() and pulp_add_plugin_bundle() reject arguments they do not
# recognise. Each case configures a throwaway project against the public helper
# (with stand-in Pulp targets) and checks the outcome: a misspelled keyword, a
# keyword this SDK does not know, a stray value and an unknown format must fail with the
# unknown-argument diagnostic; a valid call (including the separately reported
# PRODUCES_MIDI) must configure.
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()
file(MAKE_DIRECTORY "${FIXTURE_DIR}")

set(_preamble
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(UnknownArgs NONE)\n"
    "add_library(pulp-format INTERFACE)\n"
    "add_library(pulp::format ALIAS pulp-format)\n"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpUtils.cmake\")\n")

function(_configure name call out_result out_text)
    set(_src "${FIXTURE_DIR}/${name}-src")
    set(_bin "${FIXTURE_DIR}/${name}-build")
    file(REMOVE_RECURSE "${_src}" "${_bin}")
    file(MAKE_DIRECTORY "${_src}")
    file(WRITE "${_src}/CMakeLists.txt" ${_preamble} "${call}\n")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${_src}" -B "${_bin}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${out_result} "${_result}" PARENT_SCOPE)
    set(${out_text} "${_text}" PARENT_SCOPE)
endfunction()

function(_expect_rejected name call needle)
    _configure(${name} "${call}" _result _text)
    if(_result EQUAL 0)
        message(FATAL_ERROR "${name}: configure succeeded; the unknown argument was ignored:\n${_text}")
    endif()
    string(FIND "${_text}" "${needle}" _at)
    if(_at LESS 0)
        message(FATAL_ERROR "${name}: failed without the unknown-argument diagnostic for '${needle}':\n${_text}")
    endif()
endfunction()

# Positive control first: the stand-in project must configure, so a rejection
# below is caused by the argument, not by a broken harness.
_configure(valid
    "pulp_add_plugin(Valid BUNDLE_ID dev.pulp.valid VERSION 1.0.0 CATEGORY Effect PRODUCES_MIDI)"
    _valid_result _valid_text)
if(NOT _valid_result EQUAL 0)
    message(FATAL_ERROR "valid call did not configure; the harness is broken:\n${_valid_text}")
endif()
string(FIND "${_valid_text}" "unknown argument" _valid_unknown)
if(NOT _valid_unknown LESS 0)
    message(FATAL_ERROR "valid call reported an unknown argument:\n${_valid_text}")
endif()
string(FIND "${_valid_text}" "PRODUCES_MIDI is ignored" _valid_midi)
if(_valid_midi LESS 0)
    message(FATAL_ERROR "PRODUCES_MIDI lost its own warning:\n${_valid_text}")
endif()

_expect_rejected(unknown-flag
    "pulp_add_plugin(Flag BUNDLE_ID dev.pulp.flag FUTURE_ONLY_OPTION)" "unknown argument(s): FUTURE_ONLY_OPTION")
_expect_rejected(misspelled-keyword
    "pulp_add_plugin(Typo BUNDLE_ID dev.pulp.typo PLUGIN_NAMEE \"Typo\")" "unknown argument(s): PLUGIN_NAMEE, Typo")
_expect_rejected(stray-value
    "pulp_add_plugin(Stray stray-value BUNDLE_ID dev.pulp.stray)" "unknown argument(s): stray-value")
# A misspelled keyword after a list keyword is swallowed as a list value; the
# format check is what catches it.
_expect_rejected(bundle-unknown
    "pulp_add_plugin_bundle(Bundle BUNDLE_ID dev.pulp.bundle FORMATS CLAP BUNDLE_GLITCH)"
    "unknown format(s): BUNDLE_GLITCH")
_expect_rejected(bundle-unknown-keyword
    "pulp_add_plugin_bundle(Bundle2 BUNDLE_GLITCH BUNDLE_ID dev.pulp.bundle FORMATS CLAP)"
    "unknown argument(s): BUNDLE_GLITCH")
_expect_rejected(lowercase-format
    "pulp_add_plugin(Lower BUNDLE_ID dev.pulp.lower FORMATS clap)" "unknown format(s): clap")

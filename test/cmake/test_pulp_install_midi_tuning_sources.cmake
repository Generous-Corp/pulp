#[[
Install-layout regression: optional MIDI tuning provider helper sources.

pulp_enable_midi_tuning_provider() lets installed-SDK consumers opt into the
Pulp provider-neutral wrappers after `pulp add` fetches the underlying
third-party packages. The helper compiles wrapper sources from
<prefix>/src/pulp/midi/ when the installed `pulp-midi` archive was not built
with those optional providers already enabled.

Inputs (passed via -D):
  PULP_BUILD_DIR — path to a configured Pulp build directory.
]]

cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR
        "test_pulp_install_midi_tuning_sources.cmake requires "
        "-DPULP_BUILD_DIR=<dir> pointing at a configured Pulp build.")
endif()
get_filename_component(PULP_BUILD_DIR "${PULP_BUILD_DIR}" ABSOLUTE)
if(NOT EXISTS "${PULP_BUILD_DIR}/CMakeCache.txt")
    message(FATAL_ERROR
        "PULP_BUILD_DIR=${PULP_BUILD_DIR} is not a configured CMake build "
        "directory (no CMakeCache.txt).")
endif()

set(_prefix "${CMAKE_CURRENT_BINARY_DIR}/pulp-install-midi-tuning-prefix")
file(REMOVE_RECURSE "${_prefix}")
file(MAKE_DIRECTORY "${_prefix}")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            --install "${PULP_BUILD_DIR}"
            --prefix  "${_prefix}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE  _stderr)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "cmake --install failed (rc=${_rc}):\n"
        "----- stdout -----\n${_stdout}\n"
        "----- stderr -----\n${_stderr}\n----- end -----")
endif()

file(GLOB_RECURSE _pulp_utils_candidates
    "${_prefix}/lib*/cmake/Pulp/PulpUtils.cmake")
file(GLOB_RECURSE _pulp_midi_tuning_candidates
    "${_prefix}/lib*/cmake/Pulp/PulpMidiTuning.cmake")
list(LENGTH _pulp_utils_candidates _pulp_utils_count)
list(LENGTH _pulp_midi_tuning_candidates _pulp_midi_tuning_count)
if(_pulp_utils_count EQUAL 0 OR _pulp_midi_tuning_count EQUAL 0)
    message(FATAL_ERROR
        "Installed PulpUtils.cmake or PulpMidiTuning.cmake not found under "
        "${_prefix}/lib*/cmake/Pulp/.")
endif()
list(GET _pulp_utils_candidates 0 _pulp_utils)
list(GET _pulp_midi_tuning_candidates 0 _pulp_midi_tuning)

file(READ "${_pulp_utils}" _pulp_utils_text)
file(READ "${_pulp_midi_tuning}" _pulp_midi_tuning_text)
if(NOT _pulp_utils_text MATCHES "PulpMidiTuning\\.cmake")
    message(FATAL_ERROR
        "Installed PulpUtils.cmake does not include PulpMidiTuning.cmake.")
endif()
if(NOT _pulp_midi_tuning_text MATCHES "function\\(pulp_enable_midi_tuning_provider")
    message(FATAL_ERROR
        "Installed PulpMidiTuning.cmake does not expose "
        "pulp_enable_midi_tuning_provider().")
endif()

set(_required_sources
    mts_esp_tuning.cpp
    scala_tuning.cpp)

set(_missing "")
foreach(_src IN LISTS _required_sources)
    set(_path "${_prefix}/src/pulp/midi/${_src}")
    if(NOT EXISTS "${_path}")
        list(APPEND _missing "${_src}")
    endif()
endforeach()

if(_missing)
    message(FATAL_ERROR
        "Installed Pulp SDK is missing optional MIDI tuning provider sources "
        "used by pulp_enable_midi_tuning_provider():\n"
        "  ${_missing}\n"
        "Expected under: ${_prefix}/src/pulp/midi/\n"
        "Update the install(FILES ...) block in tools/cmake/PulpInstallRules.cmake.")
endif()

set(_consumer_src "${CMAKE_CURRENT_BINARY_DIR}/pulp-install-midi-tuning-consumer")
set(_consumer_build "${CMAKE_CURRENT_BINARY_DIR}/pulp-install-midi-tuning-consumer-build")
file(REMOVE_RECURSE "${_consumer_src}" "${_consumer_build}")
file(MAKE_DIRECTORY "${_consumer_src}")
file(WRITE "${_consumer_src}/CMakeLists.txt"
"cmake_minimum_required(VERSION 3.24)\n"
"project(PulpMidiTuningConsumer LANGUAGES CXX)\n"
"set(CMAKE_PREFIX_PATH \"${_prefix}\")\n"
"find_package(Pulp REQUIRED CONFIG)\n"
"add_library(mts_esp_client INTERFACE IMPORTED)\n"
"add_library(sst::tuning-library INTERFACE IMPORTED)\n"
"file(WRITE \"\${CMAKE_CURRENT_BINARY_DIR}/consumer.cpp\" \"void consumer() {}\\n\")\n"
"add_library(consumer STATIC \"\${CMAKE_CURRENT_BINARY_DIR}/consumer.cpp\")\n"
"pulp_enable_midi_tuning_provider(consumer MTS_ESP SCALA)\n"
"get_target_property(_defs consumer COMPILE_DEFINITIONS)\n"
"get_target_property(_sources consumer SOURCES)\n"
"get_target_property(_links consumer LINK_LIBRARIES)\n"
"if(NOT \";\${_defs};\" MATCHES \";PULP_HAS_MTS_ESP=1;\" OR\n"
"        NOT \";\${_defs};\" MATCHES \";PULP_HAS_SCALA_TUNING=1;\")\n"
"    message(FATAL_ERROR \"consumer missing MIDI tuning compile definitions: \${_defs}\")\n"
"endif()\n"
"if(NOT _sources MATCHES \"mts_esp_tuning\\\\.cpp\" OR\n"
"        NOT _sources MATCHES \"scala_tuning\\\\.cpp\")\n"
"    message(FATAL_ERROR \"consumer missing MIDI tuning wrapper sources: \${_sources}\")\n"
"endif()\n"
"if(NOT _links MATCHES \"mts_esp_client\" OR\n"
"        NOT _links MATCHES \"sst::tuning-library\")\n"
"    message(FATAL_ERROR \"consumer missing MIDI tuning package links: \${_links}\")\n"
"endif()\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${_consumer_src}"
            -B "${_consumer_build}"
    RESULT_VARIABLE _consumer_rc
    OUTPUT_VARIABLE _consumer_stdout
    ERROR_VARIABLE  _consumer_stderr)
if(NOT _consumer_rc EQUAL 0)
    message(FATAL_ERROR
        "Installed-SDK consumer configure failed for "
        "pulp_enable_midi_tuning_provider() (rc=${_consumer_rc}):\n"
        "----- stdout -----\n${_consumer_stdout}\n"
        "----- stderr -----\n${_consumer_stderr}\n----- end -----")
endif()

message(STATUS
    "Install layout: optional MIDI tuning provider helper sources are present "
    "(${_required_sources}) and an installed-SDK consumer can configure the helper.")

#[[
Proves Pulp rejects a macOS static archive whose embedded deployment floor is
newer than the consumer target.  A fake otool keeps the fixture deterministic;
the production integration still executes Apple's real otool over Skia/Dawn.
]]

cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()

file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}")
set(_module "${PULP_SOURCE_DIR}/tools/cmake/PulpMacosArchiveFloor.cmake")
set(_archive "${FIXTURE_DIR}/libfixture.a")
file(WRITE "${_archive}" "fixture")

include("${_module}")

set(_mixed_load_commands [=[
Load command 1
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 13.0
      sdk 15.4
Load command 2
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 15.0
      sdk 15.4
]=])
_pulp_macos_parse_otool_floor(_parsed "${_mixed_load_commands}")
if(NOT _parsed STREQUAL "15.0")
    message(FATAL_ERROR "Modern load-command parser expected 15.0, got '${_parsed}'")
endif()

set(_legacy_load_commands [=[
Load command 1
      cmd LC_VERSION_MIN_MACOSX
  cmdsize 16
  version 10.13
      sdk 11.3
]=])
_pulp_macos_parse_otool_floor(_legacy "${_legacy_load_commands}")
if(NOT _legacy STREQUAL "10.13")
    message(FATAL_ERROR "Legacy load-command parser expected 10.13, got '${_legacy}'")
endif()

set(_fake_otool "${FIXTURE_DIR}/otool")
file(WRITE "${_fake_otool}" [=[#!/bin/sh
printf '%s\n' 'Load command 1' '      cmd LC_BUILD_VERSION' '  cmdsize 32' ' platform 1' "    minos ${PULP_TEST_ARCHIVE_FLOOR}" '      sdk 15.4'
]=])
file(CHMOD "${_fake_otool}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE)

set(ENV{PULP_TEST_ARCHIVE_FLOOR} "13.0")
pulp_assert_macos_archive_floor(
    TARGET "13.4" OTOOL "${_fake_otool}" ARCHIVES "${_archive}")

set(_driver "${FIXTURE_DIR}/reject.cmake")
file(WRITE "${_driver}"
    "include(\"${_module}\")\n"
    "pulp_assert_macos_archive_floor(TARGET \"13.4\" OTOOL \"${_fake_otool}\" ARCHIVES \"${_archive}\")\n")
set(ENV{PULP_TEST_ARCHIVE_FLOOR} "15.0")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${_driver}"
    RESULT_VARIABLE _reject_rc
    OUTPUT_VARIABLE _reject_out
    ERROR_VARIABLE _reject_err)
if(_reject_rc EQUAL 0)
    message(FATAL_ERROR "A macOS 15.0 archive was accepted for a macOS 13.4 target")
endif()
set(_reject_text "${_reject_out}\n${_reject_err}")
if(NOT _reject_text MATCHES "archive minimum: macOS 15.0" OR
        NOT _reject_text MATCHES "consumer target: macOS 13.4")
    message(FATAL_ERROR "Archive-floor rejection was not actionable:\n${_reject_text}")
endif()

message(STATUS "macOS prebuilt archive floor gate accepted 13.0 and rejected 15.0. OK.")

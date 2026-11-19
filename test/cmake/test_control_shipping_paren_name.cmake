# An artifact named with a parenthesised suffix ("X (Beta)") must still get a
# dependency scan. Apple's otool reads the "(...)" as archive-member syntax, so
# the scanner hands it a plain-named link. The fixture is a copy of a real
# Mach-O (CMake itself) under such a name.
cmake_minimum_required(VERSION 3.24)

file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}")
set(_scanner "${PULP_SOURCE_DIR}/tools/cmake/check_control_shipping_artifact.cmake")
find_program(_otool otool)
if(NOT _otool)
    message(FATAL_ERROR "otool is required")
endif()

set(_paren "${FIXTURE_DIR}/My Synth (Beta)")
set(_plain "${FIXTURE_DIR}/My Synth")
file(COPY_FILE "${CMAKE_COMMAND}" "${_paren}")
file(COPY_FILE "${CMAKE_COMMAND}" "${_plain}")

# Control 1: the plain name scans, through the scanner.
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DPULP_CONTROL_OTOOL_SELFTEST=${_plain}" "-DPULP_CONTROL_OTOOL_WORKDIR=${FIXTURE_DIR}/work"
    -P "${_scanner}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "libSystem")
    message(FATAL_ERROR "the plain-named control did not scan: ${_out}${_err}")
endif()

# Control 2: bare otool really does fail on the parenthesised name, so the
# fixture exercises the bug and not something else.
execute_process(COMMAND "${_otool}" -L "${_paren}" RESULT_VARIABLE _bare_rc
    OUTPUT_QUIET ERROR_QUIET)
if(_bare_rc EQUAL 0)
    message(FATAL_ERROR "bare otool accepted the parenthesised name; the fixture proves nothing")
endif()

# The scanner scans it, and leaves no link behind.
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DPULP_CONTROL_OTOOL_SELFTEST=${_paren}" "-DPULP_CONTROL_OTOOL_WORKDIR=${FIXTURE_DIR}/work"
    -P "${_scanner}" RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "libSystem")
    message(FATAL_ERROR "the parenthesised name was not scanned: ${_out}${_err}")
endif()
file(GLOB _left "${FIXTURE_DIR}/work/otool-scan-*")
if(_left)
    message(FATAL_ERROR "the scan left a link behind: ${_left}")
endif()

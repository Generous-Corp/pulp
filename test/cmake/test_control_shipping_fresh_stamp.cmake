# The persisted control-shipping stamp edge skips a rescan only when POST_BUILD
# already scanned the exact inputs. The fixture ARTIFACT is not a binary, so
# any path that really scans it fails; a clean exit therefore proves the skip,
# and a failure proves a rescan happened.
cmake_minimum_required(VERSION 3.24)

file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}")
# A private copy of the scanner and of the forbidden-marker list it reads from
# its own directory, so a policy change (a newer scanner, a new marker) can be
# simulated without touching the source tree.
set(_scanner "${FIXTURE_DIR}/check_control_shipping_artifact.cmake")
set(_list "${FIXTURE_DIR}/release_artifact_forbidden.json")
file(COPY_FILE "${PULP_SOURCE_DIR}/tools/cmake/check_control_shipping_artifact.cmake"
    "${_scanner}")
file(COPY_FILE "${PULP_SOURCE_DIR}/tools/cmake/PulpReleaseArtifactScan.cmake"
    "${FIXTURE_DIR}/PulpReleaseArtifactScan.cmake")
file(COPY_FILE "${PULP_SOURCE_DIR}/tools/cmake/release_artifact_forbidden.json"
    "${_list}")
set(_artifact "${FIXTURE_DIR}/artifact.bin")
set(_manifest "${FIXTURE_DIR}/manifest.json")
set(_shipping "${FIXTURE_DIR}/shipping.json")
set(_report "${FIXTURE_DIR}/report.json")
set(_stamp "${FIXTURE_DIR}/scan.stamp")

function(_settle)
    # Distinct timestamps even on filesystems with one-second resolution.
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
endfunction()

function(_scan expect_success label)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        "-DARTIFACT=${_artifact}" "-DMANIFEST=${_manifest}"
        "-DSHIPPING_MANIFEST=${_shipping}" "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DREPORT=${_report}" "-DSKIP_IF_FRESH_STAMP=${_stamp}"
        -P "${_scanner}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(expect_success AND NOT _rc EQUAL 0)
        message(FATAL_ERROR "${label}: expected the fresh stamp to skip the scan: ${_out}${_err}")
    endif()
    if(NOT expect_success AND _rc EQUAL 0)
        message(FATAL_ERROR "${label}: expected a real scan (which rejects the fixture), got a skip")
    endif()
endfunction()

file(WRITE "${_artifact}" "not a binary")
file(WRITE "${_manifest}" "{}")
file(WRITE "${_shipping}" "{}")
file(WRITE "${_report}" "{}")

# Control: no stamp yet, so the scanner must really run and reject the fixture.
_scan(FALSE "missing stamp")

_settle()
file(TOUCH "${_stamp}")
_scan(TRUE "stamp newer than every input")

# The report is part of the evidence; without it the scan must run.
file(REMOVE "${_report}")
_scan(FALSE "missing report")
file(WRITE "${_report}" "{}")
_settle()
file(TOUCH "${_stamp}")
_scan(TRUE "report restored under a newer stamp")

foreach(_input IN ITEMS artifact manifest shipping scanner list)
    _settle()
    file(TOUCH "${_${_input}}")
    _scan(FALSE "${_input} newer than stamp")
    _settle()
    file(TOUCH "${_stamp}")
    _scan(TRUE "stamp refreshed after ${_input}")
endforeach()

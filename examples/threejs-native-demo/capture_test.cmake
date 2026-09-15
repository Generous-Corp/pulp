# CTest helper for #542: run pulp-threejs-native-demo --demo <mode> --capture
# and verify the process exits bounded (not hanging at status: 'starting').
#
# Inputs (set via -D):
#   DEMO_BIN      : path to the built pulp-threejs-native-demo binary
#   CAPTURE_PATH  : path where the demo should write the PNG
#   DEMO_MODE     : optional demo mode, defaults to cube
#   REQUIRE_CAPTURE : when truthy, a missing V8 or Dawn adapter is a FAILURE
#                     rather than a tolerated skip
#
# Pass criteria (default, tolerant mode):
#   - Binary exits within the test TIMEOUT (CTest enforces 30s externally).
#   - If the build has V8 linked AND a native Dawn adapter is available,
#     the PNG file must exist and be non-empty.
#   - If V8 is unavailable or the Dawn adapter is unavailable, the binary
#     prints an explanatory message on stderr and exits 1 without hanging.
#     The tolerant mode reports that as a SKIP of the PNG assertion, not as
#     evidence the capture works: it guards only against the hang.
#
# Pass criteria (REQUIRE_CAPTURE mode):
#   - No skip path. A build without V8, or a host without a native Dawn
#     adapter, FAILS. Use this where the capture is genuinely expected to
#     produce a PNG, so an environment that quietly cannot capture cannot
#     report the same green as an environment that captured successfully.

if(NOT DEFINED DEMO_BIN)
    message(FATAL_ERROR "capture_test.cmake: DEMO_BIN not set")
endif()
if(NOT DEFINED CAPTURE_PATH)
    message(FATAL_ERROR "capture_test.cmake: CAPTURE_PATH not set")
endif()
if(NOT DEFINED DEMO_MODE)
    set(DEMO_MODE "cube")
endif()
if(REQUIRE_CAPTURE)
    set(_capture_required TRUE)
else()
    set(_capture_required FALSE)
endif()

# Clean stale capture from previous runs so a success/failure signal is real.
if(EXISTS "${CAPTURE_PATH}")
    file(REMOVE "${CAPTURE_PATH}")
endif()

# Run the demo. TIMEOUT here is a belt-and-braces guard; the outer
# set_tests_properties TIMEOUT is the authoritative hang detector.
execute_process(
    COMMAND "${DEMO_BIN}" --demo "${DEMO_MODE}" --capture "${CAPTURE_PATH}"
    TIMEOUT 20
    RESULT_VARIABLE demo_result
    OUTPUT_VARIABLE demo_stdout
    ERROR_VARIABLE demo_stderr
)

message(STATUS "threejs-native-demo stdout:\n${demo_stdout}")
if(demo_stderr)
    message(STATUS "threejs-native-demo stderr:\n${demo_stderr}")
endif()

# A string/integer RESULT_VARIABLE means timeout or signal — that IS the hang.
if(NOT demo_result MATCHES "^-?[0-9]+$")
    message(FATAL_ERROR
        "#542 regression: pulp-threejs-native-demo --demo ${DEMO_MODE} --capture "
        "did not exit within the bounded timeout (result='${demo_result}').")
endif()

# Environment-skip cases are not hangs. In the tolerant mode they skip the PNG
# assertion; under REQUIRE_CAPTURE they are failures, because a build that
# cannot capture at all must not report the same result as one that captured.
if(demo_stderr MATCHES "V8 is required")
    if(_capture_required)
        message(FATAL_ERROR
            "REQUIRE_CAPTURE: build lacks V8, so --demo ${DEMO_MODE} --capture "
            "cannot produce a PNG (stderr: ${demo_stderr})")
    endif()
    message(STATUS "SKIP (tolerant mode): PNG assertion not run, build lacks V8.")
    return()
endif()
if(demo_stderr MATCHES "Native Dawn adapter unavailable")
    if(_capture_required)
        message(FATAL_ERROR
            "REQUIRE_CAPTURE: no native Dawn adapter on this host, so "
            "--demo ${DEMO_MODE} --capture cannot produce a PNG "
            "(stderr: ${demo_stderr})")
    endif()
    message(STATUS
        "SKIP (tolerant mode): PNG assertion not run, no native Dawn adapter.")
    return()
endif()

# With V8 + Dawn present, exit non-zero or empty PNG = real failure.
if(NOT demo_result EQUAL 0)
    message(FATAL_ERROR
        "pulp-threejs-native-demo exited ${demo_result} "
        "(stderr: ${demo_stderr})")
endif()

if(NOT EXISTS "${CAPTURE_PATH}")
    message(FATAL_ERROR "Capture PNG not written: ${CAPTURE_PATH}")
endif()

file(SIZE "${CAPTURE_PATH}" capture_size)
if(capture_size LESS 64)
    message(FATAL_ERROR
        "Capture PNG is suspiciously small (${capture_size} bytes): ${CAPTURE_PATH}")
endif()

message(STATUS "threejs_native_demo_capture_no_hang (${DEMO_MODE}): OK (${capture_size} bytes)")

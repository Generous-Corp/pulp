# CTest helper for #542: run pulp-threejs-native-demo --demo cube --capture
# and verify the process exits bounded (not hanging at status: 'starting').
#
# Inputs (set via -D):
#   DEMO_BIN      : path to the built pulp-threejs-native-demo binary
#   CAPTURE_PATH  : path where the demo should write the PNG
#
# Pass criteria:
#   - Binary exits within the test TIMEOUT (CTest enforces 30s externally).
#   - If the build has V8 linked AND a native Dawn adapter is available,
#     the PNG file must exist and be non-empty.
#   - If V8 is unavailable or the Dawn adapter is unavailable, the binary
#     prints an explanatory message on stderr and exits 1 without hanging;
#     that is still a pass for this test (the hang is the #542 regression
#     we guard against).

if(NOT DEFINED DEMO_BIN)
    message(FATAL_ERROR "capture_test.cmake: DEMO_BIN not set")
endif()
if(NOT DEFINED CAPTURE_PATH)
    message(FATAL_ERROR "capture_test.cmake: CAPTURE_PATH not set")
endif()

# Clean stale capture from previous runs so a success/failure signal is real.
if(EXISTS "${CAPTURE_PATH}")
    file(REMOVE "${CAPTURE_PATH}")
endif()

# Run the demo. TIMEOUT here is a belt-and-braces guard; the outer
# set_tests_properties TIMEOUT is the authoritative hang detector.
execute_process(
    COMMAND "${DEMO_BIN}" --demo cube --capture "${CAPTURE_PATH}"
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
        "#542 regression: pulp-threejs-native-demo --demo cube --capture "
        "did not exit within the bounded timeout (result='${demo_result}').")
endif()

# Environment-skip cases are not regressions — guard only against hangs.
if(demo_stderr MATCHES "V8 is required")
    message(STATUS "Skipping PNG assertion: build lacks V8 (no #542 regression).")
    return()
endif()
if(demo_stderr MATCHES "Native Dawn adapter unavailable")
    message(STATUS "Skipping PNG assertion: no native Dawn adapter on this host.")
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

message(STATUS "threejs_native_demo_capture_no_hang: OK (${capture_size} bytes)")

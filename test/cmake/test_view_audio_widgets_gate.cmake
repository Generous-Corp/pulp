# Runs test/fixtures/view_audio_widgets_gate as its own configure, where the
# try-compiles live. Inputs: PULP_SOURCE_DIR, PULP_BINARY_DIR, FLAGS_FILE,
# GENERATOR, CXX_COMPILER, and optionally OSX_SYSROOT / OSX_ARCHITECTURES.
foreach(_input PULP_SOURCE_DIR PULP_BINARY_DIR FLAGS_FILE GENERATOR CXX_COMPILER)
    if(NOT DEFINED ${_input})
        message(FATAL_ERROR "view-audio-widgets-gate: missing input ${_input}")
    endif()
endforeach()

set(_work "${PULP_BINARY_DIR}/view-audio-widgets-gate")
file(REMOVE_RECURSE "${_work}")
set(_args
    -S "${PULP_SOURCE_DIR}/test/fixtures/view_audio_widgets_gate"
    -B "${_work}"
    -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
    "-DWIDGETS_GATE_FLAGS_FILE=${FLAGS_FILE}")
if(OSX_SYSROOT)
    list(APPEND _args "-DCMAKE_OSX_SYSROOT=${OSX_SYSROOT}")
endif()
if(OSX_ARCHITECTURES)
    list(APPEND _args "-DCMAKE_OSX_ARCHITECTURES=${OSX_ARCHITECTURES}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${_args}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
message("${_out}")
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "view-audio-widgets-gate failed (rc=${_rc}):\n${_err}")
endif()

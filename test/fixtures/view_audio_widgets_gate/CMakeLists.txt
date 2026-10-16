# Configure-time try-compiles behind pulp-view-audio-widgets-gate.
#
# widgets.hpp declares Meter, WaveformView and SpectrumView only when
# PULP_VIEW_HAS_AUDIO_WIDGETS is defined, and only pulp::view-audio publishes
# that definition. So a translation unit compiled with an extracted unit's own
# include directories and definitions (its full usage-requirement closure, as
# the build itself computes them) must not see the audio widgets, while the same
# translation unit compiled as a consumer of pulp::view-audio must.
#
# Inputs: WIDGETS_GATE_FLAGS_FILE, written by file(GENERATE) in the Pulp build, sets
#   WIDGETS_GATE_EXTRACTED_TARGETS and, per target, WIDGETS_GATE_<target>_INCLUDES and
#   WIDGETS_GATE_<target>_DEFINITIONS, plus WIDGETS_GATE_AUDIO_INCLUDES / WIDGETS_GATE_AUDIO_DEFINITIONS
#   for a consumer of pulp::view-audio.
cmake_minimum_required(VERSION 3.24)
project(PulpViewAudioWidgetsGate LANGUAGES CXX)

if(NOT EXISTS "${WIDGETS_GATE_FLAGS_FILE}")
    message(FATAL_ERROR "view-audio-widgets-gate: missing flags file ${WIDGETS_GATE_FLAGS_FILE}")
endif()
include("${WIDGETS_GATE_FLAGS_FILE}")
if(NOT WIDGETS_GATE_EXTRACTED_TARGETS)
    message(FATAL_ERROR "view-audio-widgets-gate: no extracted targets were recorded")
endif()

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(_control_source "#include <pulp/view/widgets.hpp>\nvoid probe() { pulp::view::Knob knob; (void)knob; }\n")
set(_meter_source "#include <pulp/view/widgets.hpp>\nvoid probe() { pulp::view::Meter meter; (void)meter; }\n")

# try_compile one source with a target's flags. RESULT is TRUE when it compiled.
function(probe name includes definitions source RESULT OUTPUT)
    set(_dir "${CMAKE_BINARY_DIR}/${name}")
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${_dir}/probe.cpp" "${source}")
    set(_defines "")
    foreach(_definition IN LISTS definitions)
        if(NOT _definition STREQUAL "")
            list(APPEND _defines "-D${_definition}")
        endif()
    endforeach()
    try_compile(_compiled "${_dir}/build"
        SOURCES "${_dir}/probe.cpp"
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${includes}"
        COMPILE_DEFINITIONS ${_defines}
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        OUTPUT_VARIABLE _output)
    set(${RESULT} ${_compiled} PARENT_SCOPE)
    set(${OUTPUT} "${_output}" PARENT_SCOPE)
endfunction()

foreach(_target IN LISTS WIDGETS_GATE_EXTRACTED_TARGETS)
    set(_includes "${WIDGETS_GATE_${_target}_INCLUDES}")
    set(_definitions "${WIDGETS_GATE_${_target}_DEFINITIONS}")
    if(_definitions MATCHES "(^|;)PULP_VIEW_HAS_AUDIO_WIDGETS(=|;|$)")
        message(FATAL_ERROR
            "view-audio-widgets-gate: ${_target} inherits PULP_VIEW_HAS_AUDIO_WIDGETS: ${_definitions}")
    endif()

    # Control: the extracted unit's flags compile an ordinary view-core widget,
    # so a failure below is about the audio widgets and not about the flags.
    probe("${_target}-control" "${_includes}" "${_definitions}" "${_control_source}" _ok _out)
    if(NOT _ok)
        message(FATAL_ERROR
            "view-audio-widgets-gate: ${_target} flags cannot compile the control TU:\n${_out}")
    endif()

    probe("${_target}-meter" "${_includes}" "${_definitions}" "${_meter_source}" _ok _out)
    if(_ok)
        message(FATAL_ERROR
            "view-audio-widgets-gate: a TU with ${_target}'s flags can name pulp::view::Meter")
    endif()
    if(NOT _out MATCHES "Meter")
        message(FATAL_ERROR
            "view-audio-widgets-gate: ${_target} Meter probe failed for an unrelated reason:\n${_out}")
    endif()
    message(STATUS "view-audio-widgets-gate: ${_target}: control compiles, Meter is not declared")
endforeach()

if(NOT WIDGETS_GATE_AUDIO_DEFINITIONS MATCHES "(^|;)PULP_VIEW_HAS_AUDIO_WIDGETS=1(;|$)")
    message(FATAL_ERROR
        "view-audio-widgets-gate: a pulp::view-audio consumer does not receive PULP_VIEW_HAS_AUDIO_WIDGETS: ${WIDGETS_GATE_AUDIO_DEFINITIONS}")
endif()
probe("view-audio-consumer-meter" "${WIDGETS_GATE_AUDIO_INCLUDES}" "${WIDGETS_GATE_AUDIO_DEFINITIONS}"
    "${_meter_source}" _ok _out)
if(NOT _ok)
    message(FATAL_ERROR
        "view-audio-widgets-gate: a pulp::view-audio consumer cannot name pulp::view::Meter:\n${_out}")
endif()
message(STATUS "view-audio-widgets-gate: PASS")

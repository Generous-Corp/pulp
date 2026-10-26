# Configure-time link boundary test for the generic view core. Its source
# half, the include edges, is pulp-view-core-include-boundary
# (tools/scripts/include_boundary.py): a clean link list cannot hide a
# forbidden header edge, and a clean source tree cannot hide a forbidden
# direct target edge.

foreach(_input PULP_SOURCE_DIR PULP_VIEW_BOUNDARY_LINKS_FILE PULP_VELLUM_POLICY
        PULP_VIEW_AUDIO_INTERFACE_DEFINITIONS_FILE)
    if(NOT DEFINED ${_input})
        message(FATAL_ERROR "pulp-view-core-audio-free: missing test input ${_input}")
    endif()
endforeach()

# The links are recorded after the whole configure has run, so a link a later
# directory adds is seen.
if(NOT EXISTS "${PULP_VIEW_BOUNDARY_LINKS_FILE}")
    message(FATAL_ERROR
        "pulp-view-core-audio-free: ${PULP_VIEW_BOUNDARY_LINKS_FILE} was not recorded at configure time")
endif()
include("${PULP_VIEW_BOUNDARY_LINKS_FILE}")
foreach(_input PULP_VIEW_CORE_LINKS PULP_VIEW_LINKS PULP_VIEW_NATIVE_LINKS
        PULP_VIEW_PLATFORM_LINKS PULP_VIEW_AUDIO_LINKS)
    if(NOT DEFINED ${_input})
        message(FATAL_ERROR "pulp-view-core-audio-free: ${PULP_VIEW_BOUNDARY_LINKS_FILE} sets no ${_input}")
    endif()
endforeach()

# The retained subsystems, from the one table every extraction check reads.
file(READ "${PULP_VELLUM_POLICY}" _policy)
string(JSON _policy_count LENGTH "${_policy}" forbidden_subsystems)
if(_policy_count LESS 1)
    message(FATAL_ERROR "pulp-view-core-audio-free: ${PULP_VELLUM_POLICY} names no subsystem")
endif()
set(_retained "")
math(EXPR _policy_last "${_policy_count} - 1")
foreach(_i RANGE ${_policy_last})
    string(JSON _name GET "${_policy}" forbidden_subsystems ${_i} name)
    string(REPLACE "_" "[-_]" _name "${_name}")
    list(APPEND _retained "${_name}")
endforeach()
list(JOIN _retained "|" _retained)

# A link item is a name, possibly namespaced (pulp::, Pulp:: from an installed
# SDK) and possibly inside generator expressions ($<LINK_ONLY:...>,
# $<$<CONFIG:Debug>:...>), so a name is bounded by list separators, ':', '<',
# '>' or ','.
function(link_matches LINKS NAMES OUT_VAR)
    if(LINKS MATCHES "(^|[|;:<>,])(pulp::|Pulp::|pulp-)?(${NAMES})([|;:<>,]|$)")
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    else()
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    endif()
endfunction()

# The link instrument must see each edge it guards; prove it on synthetic lists
# shaped like CMake's (aliased, plain, imported and generator-expression
# wrapped names), so a regex that never matches cannot pass the checks below.
foreach(_control "pulp::canvas|pulp::audio" "pulp-host" "pulp::view-graph-editor"
        "$<LINK_ONLY:pulp::midi>" "pulp::canvas|$<$<BOOL:ON>:pulp::midi>" "Pulp::audio"
        "$<BUILD_INTERFACE:pulp-gpu-audio>" "pulp::music" "pulp::gpu_audio")
    link_matches("${_control}" "${_retained}|view-graph-editor" _hit)
    if(NOT _hit)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: link negative control '${_control}' was not detected")
    endif()
endforeach()
foreach(_control "pulp::view-audio" "$<LINK_ONLY:pulp::view-audio>" "pulp::canvas|pulp::state"
        "-framework CoreAudio" "$<BUILD_INTERFACE:yaml-cpp::yaml-cpp>")
    link_matches("${_control}" "${_retained}" _hit)
    if(_hit)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: link positive control '${_control}' matched a retained subsystem")
    endif()
endforeach()

link_matches("${PULP_VIEW_CORE_LINKS}" "${_retained}|view-audio|view-graph-editor" _hit)
if(_hit)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: forbidden pulp-view-core LINK_LIBRARIES=${PULP_VIEW_CORE_LINKS}")
endif()

# The extracted view-platform target links only extracted view units: no audio,
# MIDI or host target, no audio widget or recognition pack, and not the Pulp-kept
# umbrella or tooling layer that carry them.
set(_view_platform_forbidden
    "${_retained}|view-audio|view-graph-editor|design-import-audio-pack|view-native|view-tooling|view")
foreach(_control "pulp::view-script|pulp::view-audio" "pulp::design-import|pulp::design-import-audio-pack"
        "pulp::view-script|pulp-midi" "pulp::view-native")
    link_matches("${_control}" "${_view_platform_forbidden}" _hit)
    if(NOT _hit)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: view-platform negative control '${_control}' was not detected")
    endif()
endforeach()
link_matches("pulp::view-script|pulp::design-import|pulp::view-core" "${_view_platform_forbidden}" _hit)
if(_hit)
    message(FATAL_ERROR "pulp-view-core-audio-free: view-platform positive control was rejected")
endif()
link_matches("${PULP_VIEW_PLATFORM_LINKS}" "${_view_platform_forbidden}" _hit)
if(_hit)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: forbidden pulp-view-platform LINK_LIBRARIES=${PULP_VIEW_PLATFORM_LINKS}")
endif()

# The audio widgets reach audio/signal but never the plug-in host stack.
link_matches("${PULP_VIEW_AUDIO_LINKS}" "host|format|view-graph-editor" _hit)
if(_hit)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: pulp-view-audio reaches the host, LINK_LIBRARIES=${PULP_VIEW_AUDIO_LINKS}")
endif()

# Both umbrellas carry the audio widgets and never the host-linked graph editor.
foreach(_umbrella VIEW VIEW_NATIVE)
    set(_links "${PULP_${_umbrella}_LINKS}")
    link_matches("${_links}" "view-audio" _has_audio)
    if(NOT _has_audio)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: ${_umbrella} umbrella does not link pulp::view-audio, LINK_LIBRARIES=${_links}")
    endif()
    link_matches("${_links}" "host|view-graph-editor" _hit)
    if(_hit)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: ${_umbrella} umbrella links the host stack, LINK_LIBRARIES=${_links}")
    endif()
endforeach()

# pulp-view-audio publishes exactly one definition to its consumers: the gate
# that lets widgets.hpp include the audio widget declarations. Anything else,
# notably the design-import binder switch, would make consumer headers differ
# with the provider's own build options. The list is the configured target
# property, recorded after the whole configure has run.
function(only_widgets_gate DEFINITIONS OUT_VAR)
    if(DEFINITIONS STREQUAL "PULP_VIEW_HAS_AUDIO_WIDGETS=1")
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    else()
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    endif()
endfunction()
foreach(_control
        "PULP_VIEW_HAS_AUDIO_WIDGETS=1;PULP_VIEW_AUDIO_HAS_DESIGN_IMPORT=1"
        "PULP_VIEW_AUDIO_HAS_DESIGN_IMPORT=1;PULP_VIEW_HAS_AUDIO_WIDGETS=1"
        "PULP_VIEW_AUDIO_HAS_DESIGN_IMPORT=1"
        "")
    only_widgets_gate("${_control}" _ok)
    if(_ok)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: definition negative control '${_control}' was accepted")
    endif()
endforeach()
if(NOT EXISTS "${PULP_VIEW_AUDIO_INTERFACE_DEFINITIONS_FILE}")
    message(FATAL_ERROR
        "pulp-view-core-audio-free: ${PULP_VIEW_AUDIO_INTERFACE_DEFINITIONS_FILE} was not recorded at configure time")
endif()
file(READ "${PULP_VIEW_AUDIO_INTERFACE_DEFINITIONS_FILE}" _view_audio_interface_definitions)
only_widgets_gate("${_view_audio_interface_definitions}" _ok)
if(NOT _ok)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: pulp-view-audio INTERFACE_COMPILE_DEFINITIONS must be exactly "
        "PULP_VIEW_HAS_AUDIO_WIDGETS=1, got '${_view_audio_interface_definitions}'")
endif()

# The same rule for a definition smuggled in as a compile option.
function(options_define_nothing OPTIONS OUT_VAR)
    if(OPTIONS MATCHES "(^|[;:> ])[-/]D")
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    else()
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    endif()
endfunction()
foreach(_control "-DPULP_VIEW_AUDIO_HAS_DESIGN_IMPORT=1" "-Wall;/DFOO"
        "$<$<COMPILE_LANGUAGE:CXX>:-DFOO>")
    options_define_nothing("${_control}" _ok)
    if(_ok)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: option negative control '${_control}' was accepted")
    endif()
endforeach()
options_define_nothing("-Wall;-fno-exceptions" _ok)
if(NOT _ok)
    message(FATAL_ERROR "pulp-view-core-audio-free: option positive control was rejected")
endif()
if(NOT EXISTS "${PULP_VIEW_AUDIO_INTERFACE_DEFINITIONS_FILE}.options")
    message(FATAL_ERROR
        "pulp-view-core-audio-free: ${PULP_VIEW_AUDIO_INTERFACE_DEFINITIONS_FILE}.options was not recorded")
endif()
file(READ "${PULP_VIEW_AUDIO_INTERFACE_DEFINITIONS_FILE}.options" _view_audio_interface_options)
options_define_nothing("${_view_audio_interface_options}" _ok)
if(NOT _ok)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: pulp-view-audio INTERFACE_COMPILE_OPTIONS define a macro: "
        "'${_view_audio_interface_options}'")
endif()

message(STATUS "pulp-view-core-audio-free: PASS (view-core, view-audio and umbrella LINK_LIBRARIES; view-audio INTERFACE_COMPILE_DEFINITIONS; negative controls detected)")

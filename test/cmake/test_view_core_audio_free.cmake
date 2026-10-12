# Configure-time link boundary test for the generic view core. Its source
# half, the include edges, is pulp-view-core-include-boundary
# (tools/scripts/include_boundary.py): a clean link list cannot hide a
# forbidden header edge, and a clean source tree cannot hide a forbidden
# direct target edge.

foreach(_input PULP_SOURCE_DIR PULP_VIEW_CORE_LINKS PULP_VIEW_LINKS
        PULP_VIEW_NATIVE_LINKS PULP_VIEW_AUDIO_LINKS PULP_VIEW_AUDIO_INTERFACE_DEFINITIONS_FILE)
    if(NOT DEFINED ${_input})
        message(FATAL_ERROR "pulp-view-core-audio-free: missing test input ${_input}")
    endif()
endforeach()

function(link_matches LINKS NAMES OUT_VAR)
    if(LINKS MATCHES "(^|[|;])(pulp::|pulp-)?(${NAMES})([|;]|$)")
        set(${OUT_VAR} TRUE PARENT_SCOPE)
    else()
        set(${OUT_VAR} FALSE PARENT_SCOPE)
    endif()
endfunction()

# The link instrument must see each edge it guards; prove it on synthetic lists
# shaped like CMake's (aliased and plain names), so a regex that never matches
# cannot pass the checks below.
foreach(_control "pulp::canvas|pulp::audio" "pulp-host" "pulp::view-graph-editor")
    link_matches("${_control}" "audio|host|view-graph-editor" _hit)
    if(NOT _hit)
        message(FATAL_ERROR
            "pulp-view-core-audio-free: link negative control '${_control}' was not detected")
    endif()
endforeach()
link_matches("pulp::view-audio" "audio" _hit)
if(_hit)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: link control matched view-audio as audio")
endif()

link_matches("${PULP_VIEW_CORE_LINKS}" "signal|audio|host|midi|format|view-audio|view-graph-editor" _hit)
if(_hit)
    message(FATAL_ERROR
        "pulp-view-core-audio-free: forbidden pulp-view-core LINK_LIBRARIES=${PULP_VIEW_CORE_LINKS}")
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

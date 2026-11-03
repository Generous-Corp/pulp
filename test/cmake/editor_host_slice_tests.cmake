# The editor-host slice: one processor, its editor from an installed Vellum
# SDK through the editor-host contract, and Pulp's own canvas/render/view not
# linked at all. See test/editor_host_slice/README.md.
#
# Opt-in and macOS-only. Nothing here configures unless
# PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX names an installed Vellum SDK built
# with the mirror and the RmlUi module.

set(PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX "" CACHE PATH
    "Installed Vellum SDK (mirror + RmlUi) for the editor-host slice; empty disables it")

if(NOT APPLE OR PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX STREQUAL "")
    return()
endif()

find_package(Vellum CONFIG REQUIRED
    PATHS "${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/lib/cmake/Vellum" NO_DEFAULT_PATH)
find_package(VellumRmlUi CONFIG REQUIRED
    PATHS "${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/lib/cmake/VellumRmlUi" NO_DEFAULT_PATH)

set(_slice_dir "${CMAKE_CURRENT_SOURCE_DIR}/editor_host_slice")
set(_slice_out "${CMAKE_CURRENT_BINARY_DIR}/editor_host_slice")
set(_slice_maps "${_slice_out}/maps")
file(MAKE_DIRECTORY "${_slice_maps}")

# Every slice binary writes a link map, which the census reads to prove which
# objects were actually linked.
function(_pulp_slice_link_map target)
    target_link_options(${target} PRIVATE
        "LINKER:-map,${_slice_maps}/$<TARGET_FILE_NAME:${target}>.map")
endfunction()

# The processor: compiled once, linked unchanged into every executable.
add_library(pulp-slice-gain-meter STATIC ${_slice_dir}/gain_meter_processor.cpp)
target_link_libraries(pulp-slice-gain-meter PUBLIC pulp::format-core)

# The native-window helpers every executable shares. slice_main.cpp itself is
# compiled per executable, because it carries that executable's expectations.
add_library(pulp-slice-harness STATIC
    ${_slice_dir}/host_window_mac.mm
    ${_slice_dir}/layer_view_mac.mm)
target_link_libraries(pulp-slice-harness PUBLIC pulp-slice-gain-meter
    "-framework AppKit" "-framework QuartzCore")
set_source_files_properties(${_slice_dir}/host_window_mac.mm ${_slice_dir}/layer_view_mac.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")

# The Vellum side: its own shared library, built against the installed Vellum
# SDK and nothing from Pulp's tree. It must never link a pulp:: target.
add_library(pulp-slice-vellum-ui SHARED ${_slice_dir}/vellum_ui/knob_panel.cpp)
target_link_libraries(pulp-slice-vellum-ui PRIVATE Vellum::MirrorView_core)
set_target_properties(pulp-slice-vellum-ui PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    BUILD_RPATH "${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/mirror/lib;${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/lib")
_pulp_slice_link_map(pulp-slice-vellum-ui)

# The RmlUi document, embedded as a string.
file(READ "${_slice_dir}/knob_led.rml" _slice_rml)
file(WRITE "${_slice_out}/gen/slice_rmlui_document.hpp"
    "#pragma once\ninline constexpr const char* kSliceRmlDocument = R\"rml(${_slice_rml})rml\";\n")

foreach(_kind headless vellum rmlui)
    add_executable(pulp-slice-${_kind} ${_slice_dir}/slice_main.cpp)
    target_link_libraries(pulp-slice-${_kind} PRIVATE pulp-slice-harness)
    _pulp_slice_link_map(pulp-slice-${_kind})
endforeach()

target_sources(pulp-slice-headless PRIVATE ${_slice_dir}/headless_editor.cpp)
target_compile_definitions(pulp-slice-headless PRIVATE
    SLICE_KIND="headless" SLICE_EXPECT_EDITOR=0 SLICE_EXACT_DRAG=0 SLICE_HAS_LED=0
    SLICE_VALUE_KEY="none")

target_sources(pulp-slice-vellum PRIVATE ${_slice_dir}/vellum_editor.cpp)
target_link_libraries(pulp-slice-vellum PRIVATE pulp-slice-vellum-ui)
target_compile_definitions(pulp-slice-vellum PRIVATE
    SLICE_KIND="vellum" SLICE_EXPECT_EDITOR=1 SLICE_EXACT_DRAG=0 SLICE_HAS_LED=0
    SLICE_VALUE_KEY="knob")

target_sources(pulp-slice-rmlui PRIVATE ${_slice_dir}/rmlui_editor.cpp)
target_include_directories(pulp-slice-rmlui PRIVATE "${_slice_out}/gen")
target_link_libraries(pulp-slice-rmlui PRIVATE VellumRmlUi::SoftRenderer)
target_compile_definitions(pulp-slice-rmlui PRIVATE
    SLICE_KIND="rmlui" SLICE_EXPECT_EDITOR=1 SLICE_EXACT_DRAG=1 SLICE_HAS_LED=1
    SLICE_VALUE_KEY="gain"
    SLICE_GOLDEN_PNG="${_slice_dir}/knob_led.golden.png")
set_target_properties(pulp-slice-rmlui PROPERTIES
    BUILD_RPATH "${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/lib")
pulp_test_data(pulp-slice-rmlui NO_DEFINE PATHS test/editor_host_slice/knob_led.golden.png)

foreach(_kind headless vellum rmlui)
    add_test(NAME pulp-editor-host-slice-${_kind}
        COMMAND pulp-slice-${_kind} "${_slice_out}/run")
    set_tests_properties(pulp-editor-host-slice-${_kind} PROPERTIES
        LABELS "editor-host-slice" FIXTURES_SETUP slice-${_kind})
endforeach()

add_test(NAME pulp-editor-host-slice-audio-identical
    COMMAND ${CMAKE_COMMAND} -DRUN_DIR=${_slice_out}/run
        -P ${_slice_dir}/compare_audio.cmake)
set_tests_properties(pulp-editor-host-slice-audio-identical PROPERTIES
    LABELS "editor-host-slice"
    FIXTURES_REQUIRED "slice-headless;slice-vellum;slice-rmlui")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME pulp-editor-host-slice-symbols
    COMMAND ${Python3_EXECUTABLE} ${_slice_dir}/check_symbols.py
        --headless $<TARGET_FILE:pulp-slice-headless>
        --vellum $<TARGET_FILE:pulp-slice-vellum>
        --rmlui $<TARGET_FILE:pulp-slice-rmlui>
        --vellum-ui $<TARGET_FILE:pulp-slice-vellum-ui>
        --map-dir ${_slice_maps}
        --vellum-prefix ${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX})
set_tests_properties(pulp-editor-host-slice-symbols PROPERTIES LABELS "editor-host-slice")

add_custom_target(pulp-editor-host-slice DEPENDS
    pulp-slice-headless pulp-slice-vellum pulp-slice-rmlui pulp-slice-vellum-ui)

# The editor-host slice: one processor, its editor from an installed Vellum
# SDK through the editor-host contract, and Pulp's own canvas/render/view not
# linked at all. See test/editor_host_slice/README.md.
#
# Opt-in and macOS-only. Nothing here configures unless
# PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX names an installed Vellum SDK built
# with the mirror and the RmlUi module.

# The acceptance runner's verdict logic (tools/scripts/editor_host_acceptance.py)
# needs neither a build of the slice nor a Vellum SDK, so it is tested always.
if(Python3_EXECUTABLE)
    add_test(NAME editor-host-acceptance-selftest
        COMMAND ${Python3_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/tools/scripts/test_editor_host_acceptance.py")
    set_tests_properties(editor-host-acceptance-selftest PROPERTIES LABELS "editor-host")
endif()

set(PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX "" CACHE PATH
    "Installed Vellum SDK (mirror + RmlUi) for the editor-host slice; empty disables it")

if(NOT APPLE OR PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX STREQUAL "")
    return()
endif()

find_package(Vellum CONFIG REQUIRED
    PATHS "${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/lib/cmake/Vellum" NO_DEFAULT_PATH)
# VellumRenderer (the RmlUi renderer on Vellum's own graphics) is optional: an
# SDK built without VELLUM_RMLUI_VELLUM_RENDERER does not carry it.
find_package(VellumRmlUi CONFIG REQUIRED OPTIONAL_COMPONENTS VellumRenderer
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

# The RmlUi documents, embedded as strings: the knob panel, and the module's
# empty document for the golden image's negative control.
file(READ "${_slice_dir}/knob_led.rml" _slice_rml)
file(READ "${_slice_dir}/empty.rml" _slice_empty_rml)
file(WRITE "${_slice_out}/gen/slice_rmlui_document.hpp"
    "#pragma once\ninline constexpr const char* kSliceRmlDocument = R\"rml(${_slice_rml})rml\";\n"
    "inline constexpr const char* kSliceEmptyRmlDocument = R\"rml(${_slice_empty_rml})rml\";\n")

foreach(_kind headless vellum rmlui native)
    add_executable(pulp-slice-${_kind} ${_slice_dir}/slice_main.cpp)
    target_link_libraries(pulp-slice-${_kind} PRIVATE pulp-slice-harness)
    _pulp_slice_link_map(pulp-slice-${_kind})
endforeach()

target_sources(pulp-slice-headless PRIVATE ${_slice_dir}/headless_editor.cpp)
target_compile_definitions(pulp-slice-headless PRIVATE
    SLICE_KIND="headless" SLICE_TEXT_FIELD=0 SLICE_EMPTY_DOCUMENT=0 SLICE_EXPECT_EDITOR=0 SLICE_EXACT_DRAG=0 SLICE_HAS_LED=0
    SLICE_VALUE_KEY="none")

target_sources(pulp-slice-vellum PRIVATE ${_slice_dir}/vellum_editor.cpp)
target_link_libraries(pulp-slice-vellum PRIVATE pulp-slice-vellum-ui)
target_compile_definitions(pulp-slice-vellum PRIVATE
    SLICE_KIND="vellum" SLICE_TEXT_FIELD=1 SLICE_EMPTY_DOCUMENT=0 SLICE_EXPECT_EDITOR=1 SLICE_EXACT_DRAG=0 SLICE_HAS_LED=0
    SLICE_VALUE_KEY="knob")

target_sources(pulp-slice-rmlui PRIVATE ${_slice_dir}/rmlui_editor.cpp)
target_include_directories(pulp-slice-rmlui PRIVATE "${_slice_out}/gen")
target_link_libraries(pulp-slice-rmlui PRIVATE VellumRmlUi::SoftRenderer)
target_compile_definitions(pulp-slice-rmlui PRIVATE
    SLICE_KIND="rmlui" SLICE_TEXT_FIELD=0 SLICE_EMPTY_DOCUMENT=1 SLICE_EXPECT_EDITOR=1 SLICE_EXACT_DRAG=1 SLICE_HAS_LED=1
    SLICE_VALUE_KEY="gain"
    SLICE_GOLDEN_PNG="${_slice_dir}/knob_led.golden.png")
set_target_properties(pulp-slice-rmlui PROPERTIES
    BUILD_RPATH "${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/lib")
pulp_test_data(pulp-slice-rmlui NO_DEFINE PATHS test/editor_host_slice/knob_led.golden.png)

# Bring your own UI: a plain AppKit editor over the contract alone.
target_sources(pulp-slice-native PRIVATE ${_slice_dir}/native_editor.mm)
set_source_files_properties(${_slice_dir}/native_editor.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_compile_definitions(pulp-slice-native PRIVATE
    SLICE_KIND="native" SLICE_TEXT_FIELD=0 SLICE_EMPTY_DOCUMENT=0 SLICE_EXPECT_EDITOR=1 SLICE_EXACT_DRAG=1 SLICE_HAS_LED=0
    SLICE_VALUE_KEY="gain")

# Each editor inside Pulp's real VST3, CLAP and AU v2 adapters, in process, and
# as a headless standalone app. These link Pulp's format adapters (and the
# standalone app its view stack), so the symbol census does not read them.
set(_slice_editor_vellum ${_slice_dir}/vellum_editor.cpp)
set(_slice_editor_rmlui ${_slice_dir}/rmlui_editor.cpp)
set(_slice_editor_native ${_slice_dir}/native_editor.mm)
set(_slice_value_key_vellum knob)
set(_slice_value_key_rmlui gain)
set(_slice_value_key_native gain)
foreach(_kind vellum rmlui native)
    add_executable(pulp-slice-formats-${_kind}
        ${_slice_dir}/format_hosts_main.mm
        ${_slice_dir}/format_hosts_clap_entry.cpp
        ${_slice_dir}/format_hosts_au_entry.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/au_v2_contract_view.mm
        ${CMAKE_SOURCE_DIR}/core/format/src/vst3_no_editor.cpp
        ${_slice_editor_${_kind}})
    target_link_libraries(pulp-slice-formats-${_kind} PRIVATE
        pulp-slice-harness pulp::format-clap-core clap ausdk
        "-framework AudioToolbox" "-framework AudioUnit" "-framework CoreAudio")
    target_compile_definitions(pulp-slice-formats-${_kind} PRIVATE
        SLICE_KIND="${_kind}" SLICE_VALUE_KEY="${_slice_value_key_${_kind}}"
        PULP_AU_CONTRACT_VIEW_FACTORY_CLASS=PulpAUContractViewFactory_Slice_${_kind}
        PULP_AU_CONTRACT_HOST_VIEW_CLASS=PulpAUContractHostView_Slice_${_kind})
    set_target_properties(pulp-slice-formats-${_kind} PROPERTIES CXX_STANDARD 23)
    target_compile_options(pulp-slice-formats-${_kind} PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-std=gnu++23>)
    set_source_files_properties(${_slice_dir}/format_hosts_main.mm PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc")

    add_executable(pulp-slice-standalone-${_kind}
        ${_slice_dir}/standalone_main.cpp ${_slice_editor_${_kind}})
    target_link_libraries(pulp-slice-standalone-${_kind} PRIVATE
        pulp-slice-gain-meter pulp-slice-harness pulp::standalone)
    target_compile_definitions(pulp-slice-standalone-${_kind} PRIVATE SLICE_KIND="${_kind}")
endforeach()
target_link_libraries(pulp-slice-formats-vellum PRIVATE pulp-slice-vellum-ui)
target_link_libraries(pulp-slice-standalone-vellum PRIVATE pulp-slice-vellum-ui)
foreach(_target pulp-slice-formats-vellum pulp-slice-standalone-vellum)
    set_target_properties(${_target} PROPERTIES
        BUILD_RPATH "${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/mirror/lib;${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/lib")
endforeach()
foreach(_target pulp-slice-formats-rmlui pulp-slice-standalone-rmlui)
    target_include_directories(${_target} PRIVATE "${_slice_out}/gen")
    target_link_libraries(${_target} PRIVATE VellumRmlUi::SoftRenderer)
    set_target_properties(${_target} PROPERTIES
        BUILD_RPATH "${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/lib")
endforeach()
foreach(_target pulp-slice-formats-native pulp-slice-standalone-native)
    target_link_libraries(${_target} PRIVATE "-framework AppKit")
endforeach()

foreach(_kind vellum rmlui native)
    add_test(NAME pulp-editor-host-formats-${_kind}
        COMMAND pulp-slice-formats-${_kind})
    add_test(NAME pulp-editor-host-standalone-${_kind}
        COMMAND pulp-slice-standalone-${_kind} "${_slice_out}/run")
    set_tests_properties(pulp-editor-host-formats-${_kind} pulp-editor-host-standalone-${_kind}
        PROPERTIES LABELS "editor-host-slice")
endforeach()

foreach(_kind headless vellum rmlui native)
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

# Acceptance item 3c: the knob document through VellumRenderer, within a stated
# tolerance of the soft renderer's golden frame. Without the component a marker
# test says so, and the runner reports 3c as SKIP with that reason.
set(_slice_renderer_targets "")
if(TARGET VellumRmlUi::VellumRenderer)
    add_executable(pulp-slice-rmlui-vellum-renderer ${_slice_dir}/rmlui_vellum_renderer_main.cpp)
    target_compile_features(pulp-slice-rmlui-vellum-renderer PRIVATE cxx_std_20)
    target_include_directories(pulp-slice-rmlui-vellum-renderer PRIVATE "${_slice_out}/gen")
    target_link_libraries(pulp-slice-rmlui-vellum-renderer PRIVATE
        VellumRmlUi::Core VellumRmlUi::SoftRenderer VellumRmlUi::VellumRenderer
        Vellum::DawnHeaders)
    target_compile_definitions(pulp-slice-rmlui-vellum-renderer PRIVATE
        SLICE_GOLDEN_PNG="${_slice_dir}/knob_led.golden.png")
    set_target_properties(pulp-slice-rmlui-vellum-renderer PROPERTIES
        OSX_ARCHITECTURES arm64
        BUILD_RPATH "${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/mirror/lib;${PULP_EDITOR_HOST_SLICE_VELLUM_PREFIX}/lib")
    add_test(NAME pulp-editor-host-slice-rmlui-renderer COMMAND pulp-slice-rmlui-vellum-renderer)
    set_tests_properties(pulp-editor-host-slice-rmlui-renderer PROPERTIES
        LABELS "editor-host-slice")
    list(APPEND _slice_renderer_targets pulp-slice-rmlui-vellum-renderer)
else()
    add_test(NAME pulp-editor-host-slice-rmlui-renderer-absent
        COMMAND ${CMAKE_COMMAND} -E echo
            "Vellum SDK built without VELLUM_RMLUI_VELLUM_RENDERER")
    set_tests_properties(pulp-editor-host-slice-rmlui-renderer-absent PROPERTIES
        LABELS "editor-host-slice")
endif()

add_custom_target(pulp-editor-host-slice DEPENDS ${_slice_renderer_targets}
    pulp-slice-headless pulp-slice-vellum pulp-slice-rmlui pulp-slice-native pulp-slice-vellum-ui
    pulp-slice-formats-vellum pulp-slice-formats-rmlui pulp-slice-formats-native
    pulp-slice-standalone-vellum pulp-slice-standalone-rmlui pulp-slice-standalone-native)

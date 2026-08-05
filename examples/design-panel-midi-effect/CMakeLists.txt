# A plugin whose editor is an imported design, built for every format.
#
# Point PULP_DESIGN_ARP_IR at any DesignIR JSON to build that panel into the
# plugin. The IR is EMBEDDED rather than read from disk at runtime, because a
# takeaway plugin has no importer around it and a path that resolves during the
# build is not a path that resolves on another machine.

set(PULP_DESIGN_ARP_IR "${CMAKE_CURRENT_SOURCE_DIR}/panel.design.pulp.json"
    CACHE FILEPATH "DesignIR JSON to embed in the design-arp plugin")

if(NOT PULP_HAS_SKIA OR NOT EXISTS "${PULP_DESIGN_ARP_IR}")
    # No Skia means no native design render; no IR means nothing to draw. Both
    # are ordinary configurations, so this stays a skip rather than an error.
    return()
endif()

# Embed the IR as a raw string literal. The delimiter matches Forge's exporter
# so a JSON body containing `)"` cannot terminate it early.
file(READ "${PULP_DESIGN_ARP_IR}" _design_arp_ir)
set(_design_panel_header
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_design_ir.hpp")
file(WRITE "${_design_panel_header}"
"#pragma once\n"
"// GENERATED from ${PULP_DESIGN_ARP_IR} — do not edit.\n"
"namespace pulp::examples {\n"
"inline constexpr const char* kEmbeddedDesignIr = R\"forgeir(\n"
"${_design_arp_ir}\n"
")forgeir\";\n"
"}  // namespace pulp::examples\n")

set(PULP_DESIGN_ARP_FORMATS VST3 CLAP Standalone)
if(APPLE)
    list(APPEND PULP_DESIGN_ARP_FORMATS AU AUv3)
endif()

pulp_add_plugin(PulpDesignArp
    FORMATS         ${PULP_DESIGN_ARP_FORMATS}
    PLUGIN_NAME     "PulpDesignArp"
    BUNDLE_ID       "com.pulp.design-arp"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY         MidiEffect
    PLUGIN_CODE     "PDar"
    MANUFACTURER_CODE "Pulp"
)

# AUv3 fans out into several targets; some are utility targets that cannot
# carry include dirs, so the type is checked rather than assumed.
foreach(_fmt IN ITEMS VST3 CLAP Standalone AU AUv3 AUv3Framework AUv3Host)
    if(TARGET PulpDesignArp_${_fmt})
        get_target_property(_kind PulpDesignArp_${_fmt} TYPE)
    endif()
    if(TARGET PulpDesignArp_${_fmt} AND NOT _kind STREQUAL "UTILITY")
        target_include_directories(PulpDesignArp_${_fmt} PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(PulpDesignArp_${_fmt} PRIVATE pulp::view)
    endif()
endforeach()

# The standalone renders its editor with --screenshot and exits before any
# audio device is opened, so a headless check is never audible.
if(TARGET PulpDesignArp_Standalone)
    target_sources(PulpDesignArp_Standalone PRIVATE main.cpp)
endif()

# ── The audio proof ───────────────────────────────────────────────────────
# Every other check here — dlopen, auval, clap-validator, headless screenshot —
# passes on a plugin that copies input to output and whose knobs drive nothing.
# These are the assertions that class of failure cannot survive.
if(PULP_BUILD_TESTS)
    add_executable(pulp-design-arp-test test_design_panel_midi_effect.cpp)
    target_include_directories(pulp-design-arp-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(pulp-design-arp-test PRIVATE
        pulp::format pulp::view pulp::state pulp::signal
        Catch2::Catch2WithMain)
    target_compile_features(pulp-design-arp-test PRIVATE cxx_std_20)
    catch_discover_tests(pulp-design-arp-test)
endif()

# ── The editor, rendered headlessly ───────────────────────────────────────
# Subject is Processor::create_view() — the same call every format adapter
# makes — so the PNG is what the plugin's editor draws, not what the importer
# would have drawn.
add_executable(pulp-design-arp-render-editor render_editor.cpp)
target_include_directories(pulp-design-arp-render-editor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pulp-design-arp-render-editor PRIVATE
    pulp::format pulp::view)
target_compile_features(pulp-design-arp-render-editor PRIVATE cxx_std_20)

# The same editor, obtained the way the adapters obtain it. A regression
# BETWEEN the adapter and create_view() fails here and passes above.
add_executable(pulp-design-arp-render-adapter-editor render_adapter_editor.cpp)
target_include_directories(pulp-design-arp-render-adapter-editor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(pulp-design-arp-render-adapter-editor PRIVATE
    pulp::format pulp::view pulp::state)
target_compile_features(pulp-design-arp-render-adapter-editor PRIVATE cxx_std_20)

if(PULP_HAS_CLAP AND APPLE)
    add_test(
        NAME clap-dlopen-PulpDesignArp
        COMMAND bash -c "python3 -c \"import ctypes; ctypes.CDLL('${CMAKE_BINARY_DIR}/CLAP/PulpDesignArp.clap/Contents/MacOS/PulpDesignArp'); print('OK')\""
    )
    set_tests_properties(clap-dlopen-PulpDesignArp PROPERTIES
        LABELS "validation;clap" TIMEOUT 20)
endif()

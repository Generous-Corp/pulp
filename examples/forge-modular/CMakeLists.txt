# Forge Modular — a VCV Rack module set built on Pulp.
#
# Opt-in: needs -DPULP_ENABLE_RACK=ON and a developer-supplied SDK. The Rack
# SDK is GPLv3 and is never vendored, committed or exported by Pulp.

if(NOT PULP_HAS_RACK)
    return()
endif()

set(_fm_dir "${CMAKE_CURRENT_SOURCE_DIR}")

# The layout manifests are the single source of truth: the panel SVGs, the
# plugin.json and the C++ widget placement are all generated from them, so a
# control and its label cannot drift apart.
file(GLOB _fm_manifests "${_fm_dir}/modules/*.json")
set(_fm_generated "${_fm_dir}/src/generated_modules.hpp")

add_custom_command(
    OUTPUT "${_fm_generated}" "${_fm_dir}/plugin.json"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/rack/forge_modular.py"
            all ${_fm_manifests}
    DEPENDS ${_fm_manifests}
            "${CMAKE_SOURCE_DIR}/tools/rack/forge_modular.py"
            "${_fm_dir}/design/glyphs.json"
    COMMENT "Forge Modular: emitting panels, plugin.json and widget placement"
    VERBATIM)
add_custom_target(forge-modular-codegen DEPENDS "${_fm_generated}")

pulp_add_rack_plugin(forge-modular
    SLUG "ForgeModular"
    VERSION "2.0.0"
    MANIFEST "${_fm_dir}/plugin.json"
    RES_DIR "${_fm_dir}/res"
    LICENSES "${_fm_dir}/LICENSE.md" "${_fm_dir}/LICENSE-MIT.txt"
    SOURCES
        "${_fm_dir}/src/plugin.cpp"
        "${_fm_dir}/src/VCO.cpp"
        "${_fm_dir}/src/MIX.cpp")

add_dependencies(forge-modular forge-modular-codegen)

# Pulp's DSP arrives as headers only — the modules use the per-sample
# generators (OscillatorT::next(), AdsrT::next()) directly, so nothing links.
target_include_directories(forge-modular PRIVATE
    "${CMAKE_SOURCE_DIR}/core/signal/include"
    "${CMAKE_SOURCE_DIR}/core/format/include"
    "${CMAKE_SOURCE_DIR}/core/audio/include"
    "${CMAKE_SOURCE_DIR}/core/state/include"
    "${CMAKE_SOURCE_DIR}/core/platform/include"
    "${CMAKE_SOURCE_DIR}/core/runtime/include")

# ── The app and the loadable plugins ─────────────────────────────────────────
#
# All four formats, because Rack Pro ships in all of them: whichever format a
# person's DAW is built around, they are served. A plugin with no DSP is the
# cheapest possible thing to ship four times and Pulp's adapters already carry
# each one, so picking a subset would only save build time at the cost of
# somebody's DAW being unsupported.
#
# The plugin is an audio effect passing signal through untouched -- Rack Pro
# wants the instrument slot, and an insert puts both windows on one track. See
# DECISIONS.md.
option(FORGE_MODULAR_BUILD_APP "Build the Forge Modular app and plugins" ON)

if(FORGE_MODULAR_BUILD_APP AND COMMAND pulp_add_plugin)
    set(_fm_app "${_fm_dir}/app")

    pulp_add_plugin(forge-modular-app
        FORMATS           Standalone AU VST3 CLAP
        SOURCES           "${_fm_app}/src/shell.cpp"
                          "${_fm_app}/src/main.cpp"
        PLUGIN_NAME       "Forge Modular"
        BUNDLE_ID         "com.generous.forgemodular"
        MANUFACTURER      "Generous"
        VERSION           "0.1.0"
        CATEGORY          Effect
        PLUGIN_CODE       "FgMd"
        MANUFACTURER_CODE "Gnrs"
        DESIGN_WIDTH      1280
        DESIGN_HEIGHT     800)

    if(TARGET forge-modular-app_Core)
        target_include_directories(forge-modular-app_Core PUBLIC
            "${_fm_app}/include" "${_fm_app}")
        target_link_libraries(forge-modular-app_Core PUBLIC
            pulp::host pulp::signal
            pulp::view pulp::view-script pulp::canvas pulp::render)
        # The UI is read from disk so it can be edited and reloaded without a
        # rebuild; the packaged build embeds it and turns hot reload off.
        target_compile_definitions(forge-modular-app_Core PUBLIC
            FORGE_MODULAR_UI_DIR="${_fm_app}/ui"
            FORGE_MODULAR_UI_HOT_RELOAD=$<IF:$<CONFIG:Debug>,true,false>)
    endif()

    # A bundle that only runs on the machine that built it is worse than one
    # that fails to build, because the failure surfaces at someone else's desk.
    if(COMMAND pulp_make_bundle_relocatable)
        foreach(_fm_t forge-modular-app_Standalone forge-modular-app_AU
                      forge-modular-app_VST3 forge-modular-app_CLAP)
            if(TARGET ${_fm_t})
                pulp_make_bundle_relocatable(${_fm_t})
            endif()
        endforeach()
    endif()
endif()

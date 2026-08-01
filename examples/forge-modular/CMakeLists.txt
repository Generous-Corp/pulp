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
# CONFIGURE_DEPENDS so a manifest added AFTER configure still triggers codegen.
# Without it the glob is evaluated once and a new module is silently missing
# from the generated header while plugin.json names it -- and Rack refuses the
# whole plugin over that one mismatch, taking every other module with it.
file(GLOB _fm_manifests CONFIGURE_DEPENDS "${_fm_dir}/modules/*.json")
set(_fm_generated "${_fm_dir}/src/generated_modules.hpp")

add_custom_command(
    # plugin.hpp and plugin.cpp are outputs too: their declaration and
    # registration blocks are derived from the same manifests, so leaving them
    # out of OUTPUT lets the build think it is up to date while they drift.
    OUTPUT "${_fm_generated}" "${_fm_dir}/plugin.json"
           "${_fm_dir}/src/plugin.hpp" "${_fm_dir}/src/plugin.cpp"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/tools/rack/forge_modular.py"
            all ${_fm_manifests}
    DEPENDS ${_fm_manifests}
            "${CMAKE_SOURCE_DIR}/tools/rack/forge_modular.py"
            "${_fm_dir}/design/glyphs.json"
    COMMENT "Forge Modular: emitting panels, plugin.json and widget placement"
    VERBATIM)
add_custom_target(forge-modular-codegen DEPENDS "${_fm_generated}")

# Every module in src/, found rather than listed.
#
# Each module defines its own `modelX`, and the generated plugin.cpp calls
# addModel for every module in the manifest -- so a source left off this list
# is not a missing feature, it is an undefined symbol that fails the whole
# plugin. The list had drifted to three of twenty-eight, which links nothing.
#
# CONFIGURE_DEPENDS for the same reason the manifest glob has it: a module
# added after configure has to trigger a reconfigure, or the build carries on
# with a source list from before it existed.
file(GLOB _fm_module_srcs CONFIGURE_DEPENDS "${_fm_dir}/src/*.cpp")

pulp_add_rack_plugin(forge-modular
    SLUG "ForgeModular"
    VERSION "2.0.0"
    MANIFEST "${_fm_dir}/plugin.json"
    RES_DIR "${_fm_dir}/res"
    LICENSES "${_fm_dir}/LICENSE.md" "${_fm_dir}/LICENSE-MIT.txt"
    SOURCES ${_fm_module_srcs})

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

    pulp_add_plugin(ForgeModularApp
        FORMATS           Standalone AU VST3 CLAP
        SOURCES           "${_fm_app}/src/shell.cpp"
                          "${_fm_app}/src/engine_client.cpp"
        PLUGIN_NAME       "Forge Modular"
        BUNDLE_ID         "com.generous.forgemodular"
        MANUFACTURER      "Generous"
        VERSION           "0.1.0"
        CATEGORY          Effect
        PLUGIN_CODE       "FgMd"
        MANUFACTURER_CODE "Gnrs"
        DESIGN_WIDTH      1280
        DESIGN_HEIGHT     800)

    # main() belongs to the standalone alone. In the shared SOURCES it is
    # compiled into every format, and the plugin targets do not link the
    # standalone library -- so AU, VST3 and CLAP all failed on StandaloneApp
    # symbols they had no business referencing.
    if(TARGET ForgeModularApp_Standalone)
        target_sources(ForgeModularApp_Standalone PRIVATE "${_fm_app}/src/main.cpp")
    endif()

    # Each format exports a different entry symbol and none can be shared, so
    # one file is compiled per target with the format selected by a define.
    foreach(_fm_fmt CLAP VST3 AU)
        if(TARGET ForgeModularApp_${_fm_fmt})
            target_sources(ForgeModularApp_${_fm_fmt} PRIVATE
                "${_fm_app}/src/entries.cpp")
            target_compile_definitions(ForgeModularApp_${_fm_fmt} PRIVATE
                PULP_FORGE_MODULAR_${_fm_fmt}=1)
        endif()
    endforeach()

    if(TARGET ForgeModularApp_Core)
        target_include_directories(ForgeModularApp_Core PUBLIC
            "${_fm_app}/include" "${_fm_app}")
        target_link_libraries(ForgeModularApp_Core PUBLIC
            pulp::host pulp::signal
            pulp::view pulp::view-script pulp::canvas pulp::render)
        # The UI is read from disk so it can be edited and reloaded without a
        # rebuild. This path is into the SOURCE TREE, so it is only correct on
        # the machine that built the binary -- every bundle also carries its own
        # copy below, and shell.cpp prefers that. Without it an installed app
        # comes up blank, which is what it did on a second machine.
        target_compile_definitions(ForgeModularApp_Core PUBLIC
            FORGE_MODULAR_UI_DIR="${_fm_app}/ui"
            FORGE_MODULAR_TOOLS_DIR="${CMAKE_SOURCE_DIR}/tools/rack"
            FORGE_MODULAR_UI_HOT_RELOAD=$<IF:$<CONFIG:Debug>,true,false>)
    endif()

    # Copy the interface into each bundle so it travels with the binary.
    # resolve_ui_dir() prefers this copy and falls back to the source tree, so a
    # developer still gets hot reload while an installed app gets a UI at all.
    foreach(_fm_tgt ForgeModularApp_Standalone ForgeModularApp_AU
                    ForgeModularApp_VST3 ForgeModularApp_CLAP)
        if(TARGET ${_fm_tgt})
            add_custom_command(TARGET ${_fm_tgt} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                        "${_fm_app}/ui"
                        "$<TARGET_BUNDLE_CONTENT_DIR:${_fm_tgt}>/Resources/ui"
                # The generator has to travel too. Without it an installed app
                # could not build anything, and said nothing about why.
                COMMAND "${CMAKE_COMMAND}" -E copy_directory
                        "${CMAKE_SOURCE_DIR}/tools/rack"
                        "$<TARGET_BUNDLE_CONTENT_DIR:${_fm_tgt}>/Resources/tools"
                COMMENT "Copying Forge Modular UI and tools into ${_fm_tgt}")
        endif()
    endforeach()

    # The interaction harness. It lives with the example rather than in
    # test/cmake/ because it drives this example's own shell, and it is
    # registered at all because the two test files already sitting in
    # test/ were never wired to anything -- an unregistered test is a
    # file, not a check.
    if(PULP_BUILD_TESTS AND TARGET ForgeModularApp_Core)
        pulp_add_test_suite(pulp-test-forge-modular-shell
            SOURCES    "${_fm_dir}/test/shell_interaction.cpp"
            LIBRARIES  ForgeModularApp_Core pulp::state pulp::view pulp::format
            LABELS     "forge-modular;interaction")
    endif()

    # A bundle that only runs on the machine that built it is worse than one
    # that fails to build, because the failure surfaces at someone else's desk.
    if(COMMAND pulp_make_bundle_relocatable)
        foreach(_fm_t ForgeModularApp_Standalone ForgeModularApp_AU
                      ForgeModularApp_VST3 ForgeModularApp_CLAP)
            if(TARGET ${_fm_t})
                pulp_make_bundle_relocatable(${_fm_t})
            endif()
        endforeach()
    endif()
endif()

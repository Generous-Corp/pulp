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
    SOURCES
        "${_fm_dir}/src/plugin.cpp"
        "${_fm_dir}/src/VCO.cpp")

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

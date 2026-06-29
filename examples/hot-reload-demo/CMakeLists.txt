# Hot-Reload Demo — a reloadable shell plugin + a separately-compiled DSP "logic"
# library that hot-swaps into the running plugin (Phase 1b: DSP hot-reload in a
# real DAW). Load the shell (VST3/CLAP) in REAPER, play audio, then edit
# logic_tremolo.cpp and run rebuild_logic.sh — the DSP morphs live.

# ── Logic library (the hot-swappable DSP) ───────────────────────────────────
# Built as a MODULE that exports the reload ABI (PULP_RELOAD_LOGIC). The shell
# dlopens THIS file and swaps it on change. Kept separate from the plugin so it
# can be recompiled without touching the shell the host has loaded.
add_library(hot-reload-demo-logic MODULE logic_tremolo.cpp)
set_target_properties(hot-reload-demo-logic PROPERTIES
    PREFIX "" OUTPUT_NAME "logic" SUFFIX ".dylib"
    POSITION_INDEPENDENT_CODE ON)
target_link_libraries(hot-reload-demo-logic PRIVATE pulp::format)

# Publish the freshly-built logic to the install convention path the shell reads
# ($HOME/.pulp/hot-reload-demo/logic.dylib), so the plugin finds its DSP the
# moment a host loads it. rebuild_logic.sh refreshes the same path.
if(APPLE)
    # NB: $ENV{HOME} is the CONFIGURING user's home, captured at configure time.
    # The shell resolves the same path at runtime via getenv("HOME"); they
    # coincide for a normal single-user dev build. If the build user differs from
    # the user running the host, set PULP_RELOAD_LOGIC_PATH or rerun rebuild_logic.sh.
    set(_hrd_logic_dest "$ENV{HOME}/.pulp/hot-reload-demo")
    add_custom_command(TARGET hot-reload-demo-logic POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_hrd_logic_dest}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:hot-reload-demo-logic>" "${_hrd_logic_dest}/logic.dylib"
        COMMENT "Publishing hot-reload logic to ${_hrd_logic_dest}/logic.dylib")
endif()

# ── Shell plugin (what the host loads) ──────────────────────────────────────
set(HRD_FORMATS VST3 CLAP Standalone)
pulp_add_plugin(HotReloadDemo
    FORMATS         ${HRD_FORMATS}
    PLUGIN_NAME     "Pulp Hot-Reload Demo"
    BUNDLE_ID       "com.pulp.hot-reload-demo"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PHrd"
    MANUFACTURER_CODE "Pulp"
)

# The shell must be built (and its logic published) together.
foreach(_hrd_fmt VST3 CLAP Standalone)
    if(TARGET HotReloadDemo_${_hrd_fmt})
        add_dependencies(HotReloadDemo_${_hrd_fmt} hot-reload-demo-logic)
    endif()
endforeach()

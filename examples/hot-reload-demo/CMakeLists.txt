# Hot-Reload Demo — a reloadable shell plugin + a separately-compiled DSP "logic"
# library that hot-swaps into the running plugin (Phase 1b: DSP hot-reload in a
# real DAW). Load the shell (VST3/CLAP) in REAPER, play audio, then edit
# logic_tremolo.cpp and run rebuild_logic.sh — the DSP morphs live.

# ── Logic library (the hot-swappable DSP) ───────────────────────────────────
# The DSP half, built + published via the pulp_add_reload_logic() helper: a
# MODULE exporting the reload ABI (PULP_RELOAD_LOGIC), published to the path the
# shell watches ($HOME/.pulp/hot-reload-demo/logic.dylib). Kept separate from the
# plugin so it can be recompiled (rebuild_logic.sh) without touching the shell
# the host has loaded.
pulp_add_reload_logic(hot-reload-demo-logic
    SOURCES     logic_tremolo.cpp
    OUTPUT_NAME logic
    PUBLISH_DIR "$ENV{HOME}/.pulp/hot-reload-demo")

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

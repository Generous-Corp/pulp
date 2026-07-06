# Hot-Reload SYNTH — a reloadable INSTRUMENT shell + a separately-compiled synth
# DSP "logic" library that hot-swaps into the running instrument live. Load the
# shell (AU in Logic, VST3/CLAP in REAPER) as an instrument, hold a chord, then
# edit logic_synth.cpp + run rebuild_logic.sh — the timbre morphs live.

# ── Logic library (the hot-swappable synth DSP) ─────────────────────────────
# Default (sine) publishes to the watched path; a second SAW variant builds the
# swap target the packaged swap.sh copies in (sine <-> saw = the audible hot-swap).
pulp_add_reload_logic(hot-reload-synth-logic
    SOURCES     logic_synth.cpp
    OUTPUT_NAME logic
    PUBLISH_DIR "$ENV{HOME}/.pulp/hot-reload-synth")
pulp_add_reload_logic(hot-reload-synth-logic-saw
    SOURCES     logic_synth.cpp
    OUTPUT_NAME logic-saw)
target_compile_definitions(hot-reload-synth-logic-saw PRIVATE SYNTH_SAW=1)

# ── Shell instrument (what the host loads) ──────────────────────────────────
set(HRS_FORMATS VST3 CLAP Standalone)
if(APPLE)
    list(APPEND HRS_FORMATS AU)   # aumu (instrument), via CATEGORY Instrument below
endif()
pulp_add_plugin(HotReloadSynth
    FORMATS           ${HRS_FORMATS}
    PLUGIN_NAME       "Pulp Hot-Reload Synth"
    BUNDLE_ID         "com.pulp.hot-reload-synth"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PHrs"
    MANUFACTURER_CODE "Pulp"
)

foreach(_hrs_fmt VST3 CLAP Standalone AU)
    if(TARGET HotReloadSynth_${_hrs_fmt})
        add_dependencies(HotReloadSynth_${_hrs_fmt} hot-reload-synth-logic)
    endif()
endforeach()

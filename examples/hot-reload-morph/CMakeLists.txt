# Hot-Reload Morph — a reloadable plugin where ONE swap changes BOTH the DSP and
# the editor. Two logic variants behind the same parameter contract:
#   warm  (default)     — sine tremolo  + blue "WARM" editor (the published initial)
#   harsh (-DMORPH_HARSH)— square chop   + red  "HARSH" editor (the swap target)
# The shell watches ~/.pulp/hot-reload-morph/logic.dylib; morph.sh copies a variant
# over it and the watcher hot-swaps DSP + UI together.
#
# The logic builds its own editor (create_view), so it is THIN (RESOLVE_FROM_HOST):
# it resolves pulp::view / pulp::format from the host at dlopen rather than
# static-linking them — one copy of the SDK in the process, no duplicate ObjC
# classes. Every host that loads it uses pulp_reload_host_ui() to export those
# symbols + retain the editor ABI surface.
#
# SCOPE: UI+DSP hot-swap works IN A HOST (Logic/REAPER), not just the standalone —
# one reload flips both the editor (blue WARM / red HARSH) and the DSP (sine /
# square). Two things make a plugin *bundle* (loaded RTLD_LOCAL) resolve the thin
# logic's symbols, both provided by pulp_reload_host_ui() (see M2b,
# planning/2026-07-03-m2b-...): (1) the plugin's restrictive -exported_symbols_list
# is augmented to export the pulp:: surface (force-loading the small
# pulp-view-reload-exports keep-alive so the editor symbols aren't dead-stripped);
# (2) the thin logic is pinned to the SDK's C++ standard so the reload
# build-fingerprint matches. Verified via auval + the A/B capture below.

set(_morph_publish "$ENV{HOME}/.pulp/hot-reload-morph")

pulp_add_reload_logic(hot-reload-morph-logic-warm
    SOURCES     logic_morph.cpp
    OUTPUT_NAME logic
    PUBLISH_DIR "${_morph_publish}"
    RESOLVE_FROM_HOST)

pulp_add_reload_logic(hot-reload-morph-logic-harsh
    SOURCES     logic_morph.cpp
    OUTPUT_NAME logic-harsh
    RESOLVE_FROM_HOST)
target_compile_definitions(hot-reload-morph-logic-harsh PRIVATE MORPH_HARSH=1)

# The standalone host (an executable that exports its SDK symbols so the thin
# logic resolves them at dlopen).
set(HRM_FORMATS VST3 CLAP Standalone)
if(APPLE)
    list(APPEND HRM_FORMATS AU)   # aufx effect; UI+DSP both hot-swap in the host
endif()
pulp_add_plugin(HotReloadMorph
    FORMATS         ${HRM_FORMATS}
    PLUGIN_NAME     "Pulp Hot-Reload Morph"
    BUNDLE_ID       "com.pulp.hot-reload-morph"
    MANUFACTURER    "Pulp"
    VERSION         "1.0.0"
    CATEGORY        Effect
    PLUGIN_CODE     "PMrp"
    MANUFACTURER_CODE "Pulp")
# The morph's thin logic references pulp::view symbols (Label/View for the editor),
# so EVERY format host must export its symbols for the logic to resolve them at
# dlopen — not just the standalone (unlike the tremolo demo, whose logic uses no
# view symbols). pulp_reload_host_ui applies the dynamic-export/link + editor-ABI
# retention needed (see the SCOPE note above).
foreach(_hrm_fmt VST3 CLAP Standalone AU)
    if(TARGET HotReloadMorph_${_hrm_fmt})
        add_dependencies(HotReloadMorph_${_hrm_fmt} hot-reload-morph-logic-warm hot-reload-morph-logic-harsh)
        # pulp_reload_host_ui (M2b): export the host symbols the thin logic resolves
        # at dlopen AND force-retain the editor ABI surface (View/Label/widgets) via
        # pulp-view-reload-exports, so the morph's create_view() editor renders in a
        # HOST (Logic/REAPER) — not just the standalone. (Plain pulp_reload_host
        # dead-strips the editor symbols; a whole-archive view-core force-load pulls
        # the mac-incompatible SDL-host TU. The curated export lib avoids both.)
        pulp_reload_host_ui(HotReloadMorph_${_hrm_fmt})
    endif()
endforeach()

# Headless A/B capture tool: renders each version's editor to a PNG and its DSP
# to a WAV, driving the swap through the shell — proof that a reload changes both
# the UX (visual) and the DSP (sonic). It is a thin-logic host, so it exports its
# SDK symbols too.
if(PULP_BUILD_TESTS OR PULP_HAS_SKIA)
    add_executable(hot-reload-morph-capture capture_morph.cpp)
    target_link_libraries(hot-reload-morph-capture PRIVATE pulp::view pulp::format)
    target_include_directories(hot-reload-morph-capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(NOT WIN32)
        target_link_libraries(hot-reload-morph-capture PRIVATE ${CMAKE_DL_LIBS})
    endif()
    pulp_reload_host(hot-reload-morph-capture)
    add_dependencies(hot-reload-morph-capture
        hot-reload-morph-logic-warm hot-reload-morph-logic-harsh)
    target_compile_definitions(hot-reload-morph-capture PRIVATE
        MORPH_LOGIC_WARM="$<TARGET_FILE:hot-reload-morph-logic-warm>"
        MORPH_LOGIC_HARSH="$<TARGET_FILE:hot-reload-morph-logic-harsh>")

    # Regression: the capture must succeed and the reload must actually CHANGE the
    # DSP (warm.wav != harsh.wav). WAVs need no Skia, so this runs everywhere; the
    # editor PNGs are additionally checked to differ when Skia rendered them.
    if(PULP_BUILD_TESTS)
        add_test(NAME hot-reload-morph-swaps-dsp-and-ui
            COMMAND ${CMAKE_COMMAND}
                -DCAP=$<TARGET_FILE:hot-reload-morph-capture>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/run_capture_test.cmake)
        set_tests_properties(hot-reload-morph-swaps-dsp-and-ui PROPERTIES
            LABELS "reload;example" TIMEOUT 60)
    endif()
endif()

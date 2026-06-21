# PulpTempoSampler — tempo-matching sampler built on the offline OfflineStretch
# engine. All formats (AU / VST3 / CLAP / Standalone) via pulp_add_plugin.

set(PULP_TEMPO_FORMATS CLAP Standalone VST3)
if(APPLE)
    list(APPEND PULP_TEMPO_FORMATS AU)
endif()

pulp_add_plugin(PulpTempoSampler
    FORMATS           ${PULP_TEMPO_FORMATS}
    PLUGIN_NAME       "PulpTempoSampler"
    BUNDLE_ID         "com.pulp.tempo-sampler"
    MANUFACTURER      "Pulp"
    VERSION           "1.0.2"
    CATEGORY          Instrument
    PLUGIN_CODE       "PTsm"
    MANUFACTURER_CODE "Pulp"
    ACCEPTS_MIDI
)

# Headless editor screenshot. macOS-only dev tool: it uses the
# CoreGraphics/Skia raster capture path and would otherwise pull the Linux
# windowing stack (SDL3/X11/Wayland) into the advisory Linux lane.
if(APPLE AND (TARGET pulp::view OR TARGET Pulp::view))
    add_executable(pulp-tempo-sampler-shot screenshot.cpp)
    target_link_libraries(pulp-tempo-sampler-shot PRIVATE
        pulp::format pulp::audio pulp::signal pulp::view)
    target_include_directories(pulp-tempo-sampler-shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(PULP_BUILD_TESTS)
    add_executable(pulp-tempo-sampler-test test_pulp_tempo_sampler.cpp)
    target_link_libraries(pulp-tempo-sampler-test PRIVATE
        pulp::format pulp::audio pulp::signal Catch2::Catch2WithMain)
    # The WaveformDropView drop-target test (Apple-gated in the source) needs
    # pulp::view. Link it only on macOS so the advisory Linux test lane doesn't
    # pull the desktop windowing stack (SDL3/X11) — mirrors the screenshot tool.
    if(APPLE AND (TARGET pulp::view OR TARGET Pulp::view))
        target_link_libraries(pulp-tempo-sampler-test PRIVATE pulp::view)
    endif()
    target_include_directories(pulp-tempo-sampler-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-tempo-sampler-test)
endif()

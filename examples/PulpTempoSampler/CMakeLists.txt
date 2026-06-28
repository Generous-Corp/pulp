# PulpTempoSampler — tempo-matching sampler built on the offline OfflineStretch
# engine. Formats are CLAP, Standalone, VST3, and AU on Apple via
# pulp_add_plugin.

set(PULP_TEMPO_FORMATS CLAP Standalone VST3)
if(APPLE)
    list(APPEND PULP_TEMPO_FORMATS AU)
endif()

pulp_add_plugin(PulpTempoSampler
    FORMATS           ${PULP_TEMPO_FORMATS}
    PLUGIN_NAME       "PulpTempoSampler"
    BUNDLE_ID         "com.pulp.tempo-sampler"
    MANUFACTURER      "Pulp"
    VERSION           "1.3.0"
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
        # pulp::view for the WaveformDropView drop-target test; pulp::standalone
        # for the SettingsPanel used by the Settings-chrome test. Both are
        # Apple-gated in the source so the advisory Linux lane stays windowing-
        # stack-free.
        target_link_libraries(pulp-tempo-sampler-test PRIVATE pulp::view pulp::standalone)
    endif()
    target_include_directories(pulp-tempo-sampler-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(pulp-tempo-sampler-test
        PRIVATE PULP_TEMPO_SAMPLER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    catch_discover_tests(pulp-tempo-sampler-test)
endif()

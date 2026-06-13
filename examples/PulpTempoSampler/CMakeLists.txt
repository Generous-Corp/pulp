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
    VERSION           "1.0.0"
    CATEGORY          Instrument
    PLUGIN_CODE       "PTsm"
    MANUFACTURER_CODE "Pulp"
    ACCEPTS_MIDI
)

# Headless editor screenshot (Phase 5 UX). Needs pulp::view (+ Skia for the
# actual raster, available in the GPU build).
if(TARGET pulp::view OR TARGET Pulp::view)
    add_executable(pulp-tempo-sampler-shot screenshot.cpp)
    target_link_libraries(pulp-tempo-sampler-shot PRIVATE
        pulp::format pulp::audio pulp::signal pulp::view)
    target_include_directories(pulp-tempo-sampler-shot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if(PULP_BUILD_TESTS)
    add_executable(pulp-tempo-sampler-test test_pulp_tempo_sampler.cpp)
    target_link_libraries(pulp-tempo-sampler-test PRIVATE
        pulp::format pulp::audio pulp::signal Catch2::Catch2WithMain)
    target_include_directories(pulp-tempo-sampler-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(pulp-tempo-sampler-test)
endif()

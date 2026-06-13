# PulpTempoSampler — tempo-matching sampler: drag a loop, detect BPM + slices,
# play slices time-stretched to the host tempo via the offline OfflineStretch
# engine on a background thread. Reuses PulpSampler's SamplerVoice/SampleStore.

set(TEMPO_SAMPLER_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../PulpSampler) # sampler_components.hpp

if(PULP_HAS_CLAP)
    add_library(PulpTempoSampler_CLAP MODULE clap_entry.cpp)
    target_link_libraries(PulpTempoSampler_CLAP PRIVATE pulp::format pulp::audio pulp::signal clap)
    target_include_directories(PulpTempoSampler_CLAP PRIVATE ${TEMPO_SAMPLER_INCLUDES})
    set_target_properties(PulpTempoSampler_CLAP PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
        OUTPUT_NAME PulpTempoSampler
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/CLAP")
    if(APPLE)
        set_target_properties(PulpTempoSampler_CLAP PROPERTIES
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.pulp.tempo-sampler"
            MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0")
    endif()
endif()

if(PULP_BUILD_TESTS)
    add_executable(pulp-tempo-sampler-test test_pulp_tempo_sampler.cpp)
    target_link_libraries(pulp-tempo-sampler-test PRIVATE
        pulp::format pulp::audio pulp::signal Catch2::Catch2WithMain)
    target_include_directories(pulp-tempo-sampler-test PRIVATE ${TEMPO_SAMPLER_INCLUDES})
    catch_discover_tests(pulp-tempo-sampler-test)
endif()

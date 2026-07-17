add_library(pulp-tool-audio)
add_library(pulp::tool-audio ALIAS pulp-tool-audio)

target_include_directories(pulp-tool-audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-tool-audio PRIVATE
    src/excerpt_service.cpp
    src/model_registry.cpp
    src/service.cpp
)

target_link_libraries(pulp-tool-audio
    PUBLIC
        pulp::audio
        pulp::runtime
)

target_include_directories(pulp-tool-audio PRIVATE
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
)

# Reusable offline audio-file analysis lib (file metrics, assertions, spectrum
# analyzers + JSON artifacts). Promoted out of test/support so the shipped CLI
# can link it without any test/ dependency.
add_subdirectory(analysis)

# Offline probe that drives a locally installed AU instrument through the host
# stack and captures its output to a WAV. Measurement tooling: the plugins it
# targets are third-party and machine-local, so it is a manual dev tool and is
# not registered as a test.
if(APPLE AND TARGET pulp-host)
    add_executable(pulp-au-instrument-probe host-probe/au_instrument_probe.cpp)
    target_link_libraries(pulp-au-instrument-probe
        PRIVATE
            pulp::host
            pulp::audio
            pulp::midi
            pulp::runtime
    )
endif()

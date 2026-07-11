# pulp-audio-analysis — reusable offline audio-file analysis (MIT).
#
# The file-analysis core promoted out of test/support so the shipped `pulp`
# CLI (`pulp audio validate …`) can reuse it without linking any test/ library.
# Depends only on core/audio (BufferView + WAV decode via FormatRegistry),
# core/signal (FFT), and choc::json. This is a tool/test-tier target: the heavy
# FFT lives here and is NEVER pulled into a runtime/plugin build (harness plan
# §9 Target-boundary rule). The scenario/contract/generator helpers that need a
# Processor stay test-only in test/support and link THIS lib.
add_library(pulp-audio-analysis STATIC
    src/audio_metrics.cpp
    src/audio_assertions.cpp
    src/audio_artifacts.cpp
    src/audio_spectrum.cpp
    src/audio_doctor_artifacts.cpp
    src/latency_evidence.cpp
)
add_library(pulp::audio-analysis ALIAS pulp-audio-analysis)

target_include_directories(pulp-audio-analysis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-audio-analysis
    PUBLIC
        pulp::audio
        pulp::signal
)

target_include_directories(pulp-audio-analysis PRIVATE
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
)

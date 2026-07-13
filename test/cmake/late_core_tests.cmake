# Late core audio, MIDI, signal, runtime, and state tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# DryWetMixer + Panner pan laws
pulp_add_test_suite(pulp-test-pan-mix-laws LIBRARIES pulp::signal)

# UMP per-note + JR Clock
pulp_add_test_suite(pulp-test-ump-extensions LIBRARIES pulp::midi)

# HMAC + AEAD primitives
pulp_add_test_suite(pulp-test-hmac-aead LIBRARIES pulp::runtime)

# Compressor sidechain HPF + lookahead
pulp_add_test_suite(pulp-test-compressor-sidechain LIBRARIES pulp::signal)

# ARA scaffold validation
pulp_add_test_suite(pulp-test-ara-scaffold LIBRARIES pulp::format)

# MIDI 1.0 backend audit
pulp_add_test_suite(pulp-test-midi1-backend-audit LIBRARIES pulp::midi)

# BufferOps SIMD helpers
pulp_add_test_suite(pulp-test-buffer-ops LIBRARIES pulp::audio pulp::runtime)

# MpeVoiceTracker per-note management + assignable PNC consumption
pulp_add_test_suite(pulp-test-mpe-tracker-per-note-management LIBRARIES pulp::midi)

# Wavetable oscillator
pulp_add_test_suite(pulp-test-wavetable LIBRARIES pulp::signal)

# Resampler
pulp_add_test_suite(pulp-test-resampler LIBRARIES pulp::signal)

# Polyphase IIR half-band filter
pulp_add_test_suite(pulp-test-halfband-iir LIBRARIES pulp::signal)

# Generic Synthesiser polyphony
pulp_add_test_suite(pulp-test-synthesiser
    SOURCES test_synthesiser.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

# SampleConverter
pulp_add_test_suite(pulp-test-sample-converter LIBRARIES pulp::audio)

# AudioSource hierarchy
pulp_add_test_suite(pulp-test-audio-source LIBRARIES pulp::audio)

# Foundation DSP/runtime/state tests
pulp_add_test_suite(pulp-test-dc-blocker LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-denormal SOURCES test_denormal.cpp test_scoped_flush_denormals.cpp LIBRARIES pulp::signal)
# MF-3 null test: snap_to_zero feedback-state writes are bit-exact vs the
# snap-disabled reference.
#
# The reference runs as a SEPARATE BINARY, not a second TU in the test
# executable. snap_to_zero() and every filter that calls it are header-defined
# with external linkage, so compiling one TU with PULP_DSP_ENABLE_SNAP_TO_ZERO=0
# gave those symbols two bodies under one mangled name — an ODR violation that
# -O3 hid by inlining and -O0 resolved by silently giving BOTH TUs the snapping
# definition (the reference then never reached a subnormal, and the null test's
# teeth check failed in Debug). Keep each executable internally consistent about
# the macro.
add_executable(pulp-denormal-null-refgen denormal_null_refgen.cpp denormal_null_reference.cpp)
target_compile_definitions(pulp-denormal-null-refgen PRIVATE PULP_DSP_ENABLE_SNAP_TO_ZERO=0)
# Headers only — deliberately NOT target_link_libraries(pulp::signal): the
# filters are header-only, and linking objects compiled with the shipping
# (snap-enabled) default would reintroduce the very symbol clash this split
# exists to remove.
target_include_directories(pulp-denormal-null-refgen PRIVATE
    $<TARGET_PROPERTY:pulp::signal,INTERFACE_INCLUDE_DIRECTORIES>
    ${CMAKE_CURRENT_SOURCE_DIR})

pulp_add_test_suite(pulp-test-denormal-null SOURCES test_denormal_null.cpp denormal_null_reference.cpp denormal_null_launcher.cpp LIBRARIES pulp::signal
    COMPILE_DEFINITIONS
        PULP_DENORMAL_NULL_REFGEN="$<TARGET_FILE:pulp-denormal-null-refgen>"
        PULP_DENORMAL_NULL_REF_BLOB="${CMAKE_CURRENT_BINARY_DIR}/denormal-null-reference.bin")
add_dependencies(pulp-test-denormal-null pulp-denormal-null-refgen)
pulp_add_test_suite(pulp-test-normalisable-range LIBRARIES pulp::state)
pulp_add_test_suite(pulp-test-adsr LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-high-resolution-timer LIBRARIES pulp::runtime)

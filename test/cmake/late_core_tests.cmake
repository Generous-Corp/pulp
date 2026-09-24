# Late core audio, MIDI, signal, runtime, and state tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Grouped executables for this manifest (pulp_add_test_group in
# tools/cmake/PulpTestSuite.cmake): each member keeps its own registration
# and properties; only the binary behind them is shared. Members are grouped
# by the compile line they already had: pulp::midi, pulp::signal, pulp::audio
# (+ pulp::runtime) and pulp::runtime alone. A suite stays on its own when it
# needs its own process or compile line: the eight RT allocation probes
# (harness/rt_allocation_probe.cpp), the denormal null test that spawns its
# reference generator by path, the ARA scaffold (pulp::format), and
# skewed-range (pulp::state alone).
pulp_add_test_group(pulp-test-group-late-midi LIBRARIES pulp::midi)
pulp_add_test_group(pulp-test-group-late-signal LIBRARIES pulp::signal)
pulp_add_test_group(pulp-test-group-late-audio LIBRARIES pulp::audio pulp::runtime)
pulp_add_test_group(pulp-test-group-late-runtime LIBRARIES pulp::runtime)

# DryWetMixer + Panner pan laws
pulp_add_test_suite(pulp-test-pan-mix-laws GROUP pulp-test-group-late-signal
    LIBRARIES pulp::signal)

# UMP per-note + JR Clock
pulp_add_test_suite(pulp-test-ump-extensions GROUP pulp-test-group-late-midi
    LIBRARIES pulp::midi)

# HMAC + AEAD primitives
pulp_add_test_suite(pulp-test-hmac-aead GROUP pulp-test-group-late-runtime
    LIBRARIES pulp::runtime)

# Compressor sidechain HPF + lookahead
pulp_add_test_suite(pulp-test-compressor-sidechain GROUP pulp-test-group-late-signal
    LIBRARIES pulp::signal)

# ARA scaffold validation
pulp_add_test_suite(pulp-test-ara-scaffold LIBRARIES pulp::format)

# MIDI 1.0 backend audit
pulp_add_test_suite(pulp-test-midi1-backend-audit GROUP pulp-test-group-late-midi
    LIBRARIES pulp::midi)

# pulp/midi/message.hpp stays self-sufficient for choc_MIDI.h's unqualified
# memcmp — a compile-time guard, so this TU failing to BUILD is the failure
# mode it is designed to catch (see test_midi_message_header.cpp).
pulp_add_test_suite(pulp-test-midi-message-header GROUP pulp-test-group-late-midi
    LIBRARIES pulp::midi)

# BufferOps SIMD helpers
pulp_add_test_suite(pulp-test-buffer-ops GROUP pulp-test-group-late-audio
    LIBRARIES pulp::audio pulp::runtime)

# MpeVoiceTracker per-note management + assignable PNC consumption
pulp_add_test_suite(pulp-test-mpe-tracker-per-note-management GROUP pulp-test-group-late-midi
    LIBRARIES pulp::midi)

# Bounded MIDI utility kernels + their existing-audio-contract bridge
pulp_add_test_suite(pulp-test-midi-utility-kernels GROUP pulp-test-group-late-midi
    SOURCES test_midi_utility_routing.cpp
    LIBRARIES pulp::midi)
pulp_add_test_suite(pulp-test-midi-utility-note-length
    SOURCES test_midi_utility_note_length.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)
pulp_add_test_suite(pulp-test-midi-utility-monophonic GROUP pulp-test-group-late-midi
    SOURCES test_midi_utility_monophonic.cpp
    LIBRARIES pulp::midi)
pulp_add_test_suite(pulp-test-midi-utility-controller GROUP pulp-test-group-late-midi
    SOURCES test_midi_utility_controller.cpp
    LIBRARIES pulp::midi)
pulp_add_test_suite(pulp-test-midi-arpeggiator
    SOURCES test_midi_arpeggiator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)
# Linear multi-lane step player (survey entry 092)
pulp_add_test_suite(pulp-test-midi-step-player
    SOURCES test_midi_step_player.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)
# Note repeat, strum, humanize, chord memory, latch and note delay kernels
pulp_add_test_suite(pulp-test-midi-performance-kernels
    SOURCES test_midi_performance_kernels.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi pulp::music)
pulp_add_test_suite(pulp-test-midi-voice-modulation-adapter
    SOURCES test_midi_voice_modulation_adapter.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-voice-modulation-sources
    SOURCES test_voice_modulation_sources.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)

# Wavetable oscillator
pulp_add_test_suite(pulp-test-wavetable GROUP pulp-test-group-late-signal
    LIBRARIES pulp::signal)

# Offline recorded-cycle authoring into immutable Wavetable band stacks.
pulp_add_test_suite(pulp-test-wavetable-authoring
    SOURCES test_wavetable_authoring.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio pulp::audio-analysis)

# Resampler
pulp_add_test_suite(pulp-test-resampler GROUP pulp-test-group-late-signal
    LIBRARIES pulp::signal)

# Polyphase IIR half-band filter
pulp_add_test_suite(pulp-test-halfband-iir GROUP pulp-test-group-late-signal
    LIBRARIES pulp::signal)

# Generic Synthesiser polyphony
pulp_add_test_suite(pulp-test-synthesiser
    SOURCES test_synthesiser.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

# SampleConverter
pulp_add_test_suite(pulp-test-sample-converter GROUP pulp-test-group-late-audio
    LIBRARIES pulp::audio)

# AudioSource hierarchy
pulp_add_test_suite(pulp-test-audio-source GROUP pulp-test-group-late-audio
    LIBRARIES pulp::audio)

# Foundation DSP/runtime/state tests
pulp_add_test_suite(pulp-test-dc-blocker GROUP pulp-test-group-late-signal
    LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-denormal GROUP pulp-test-group-late-signal
    SOURCES test_denormal.cpp test_scoped_flush_denormals.cpp
    LIBRARIES pulp::signal)
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
pulp_add_test_suite(pulp-test-skewed-range LIBRARIES pulp::state)
pulp_add_test_suite(pulp-test-adsr GROUP pulp-test-group-late-signal
    LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-high-resolution-timer GROUP pulp-test-group-late-runtime
    LIBRARIES pulp::runtime)

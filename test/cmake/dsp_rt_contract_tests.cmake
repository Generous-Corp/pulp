# DSP/MIDI realtime-contract test registrations kept out of the frozen
# top-level test manifest.

pulp_add_test_suite(pulp-test-sysex-accumulator
    SOURCES test_sysex_accumulator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-ump-sysex7-reassembler
    SOURCES test_ump_sysex7_reassembler.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-processor-defaults
    SOURCES test_processor_defaults.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-raw-midi-parser
    SOURCES test_raw_midi_parser.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-running-status
    SOURCES test_running_status.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-parameter-event-queue
    SOURCES test_parameter_event_queue.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host)

pulp_add_test_suite(pulp-test-signal-rt-safety
    SOURCES test_signal_rt_safety.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::signal-fft-backend)

pulp_add_test_suite(pulp-test-multi-channel-meter
    SOURCES test_multi_channel_meter.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

pulp_add_test_suite(pulp-test-midi-message-collector
    SOURCES test_midi_message_collector.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-ump-buffer-conversion
    SOURCES test_ump_buffer_conversion.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-mpe-buffer
    SOURCES test_mpe_buffer.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-midi-subblock
    SOURCES test_midi_subblock.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-modal-bank
    SOURCES test_modal_bank.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Drives the bridged-T core through the VaDrum voice, so it needs the example's
# include dir rather than the plugin target itself (the voice is header-only).
pulp_add_test_suite(pulp-test-bridged-t-resonator
    SOURCES test_bridged_t_resonator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/va-drum
    TIMEOUT 300)

# PULP_SOURCE_DIR locates examples/modal-specs/, which the test loads as real
# on-disk spec files rather than string literals.
pulp_add_test_suite(pulp-test-modal-spec
    SOURCES test_modal_spec.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal-modal-spec
    COMPILE_DEFINITIONS PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TIMEOUT 300)

# Header-only metric over caller-supplied render callbacks; its fixtures are
# source-owned resonators, so it needs no plugin and no library.
pulp_add_test_suite(pulp-test-interaction-residual
    SOURCES test_interaction_residual.cpp
    TIMEOUT 300)

# Band-limited square-osc bank: the inharmonic-cluster primitive behind the
# metallic PulpKit voices. Recovers partials, bounds output, proves anti-alias.
pulp_add_test_suite(pulp-test-square-osc-bank
    SOURCES test_square_osc_bank.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# PulpKit tom/conga voice: a struck bridged-T retuned into the membrane band.
# Renders and measures f0/T60 inline. Reuses VaDrum's voice header, so it needs
# both the kit voices dir and the sibling va-drum dir on the include path.
pulp_add_test_suite(pulp-test-kit-toms
    SOURCES test_kit_toms.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/pulp-kit/voices
                 ${CMAKE_SOURCE_DIR}/examples/va-drum
    TIMEOUT 300)

# PulpKit metallic voices (closed/open hat, cymbal, cowbell): square-osc cluster
# through filter + envelope. Renders and measures centroid/decay inline.
pulp_add_test_suite(pulp-test-kit-metallic
    SOURCES test_kit_metallic.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples
    TIMEOUT 300)

# PulpKit noise voices (clap, maracas): gated white noise through a resonant
# band-pass. Renders and measures the flam envelope and spectral tilt inline.
pulp_add_test_suite(pulp-test-kit-noise
    SOURCES test_kit_noise.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples
    TIMEOUT 300)

# The assembled PulpKit Processor: drives the real note map through process()
# and measures per-voice discrimination. Needs the format runtime plus the kit
# and sibling va-drum headers on the include path.
pulp_add_test_suite(pulp-test-pulp-kit
    SOURCES test_pulp_kit.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format pulp::signal
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples
                 ${CMAKE_SOURCE_DIR}/examples/va-drum
    TIMEOUT 300)

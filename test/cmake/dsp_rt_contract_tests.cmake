# DSP/MIDI realtime-contract test registrations kept out of the frozen
# top-level test manifest.

pulp_add_test_suite(pulp-test-sysex-accumulator
    SOURCES test_sysex_accumulator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-ump-sysex7-reassembler
    SOURCES test_ump_sysex7_reassembler.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-ump-packet-cursor
    SOURCES test_ump_packet_cursor.cpp harness/rt_allocation_probe.cpp
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

pulp_add_test_suite(pulp-test-realtime-pitch-time-rt-safety
    SOURCES test_realtime_pitch_time_rt_safety.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::signal-fft-backend)

pulp_add_test_suite(pulp-test-signal-mod-rt-safety
    SOURCES test_signal_mod_rt_safety.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

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

pulp_add_test_suite(pulp-test-bridged-t-resonator
    SOURCES test_bridged_t_resonator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
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

# Reusable band-limited square-oscillator bank. Recover its configured
# partials, bound its output, measure alias rejection, and hold its realtime
# allocation contract without depending on an instrument implementation.
pulp_add_test_suite(pulp-test-square-osc-bank
    SOURCES test_square_osc_bank.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Percussion-synthesis shared layer. Each suite measures the property its
# component is named for -- spectral slope, decay timing, hold interval,
# vactrol asymmetry -- rather than asserting a coefficient against itself, so
# the naive transforms they use are the slow part and the timeouts are wide.
pulp_add_test_suite(pulp-test-noise-source
    SOURCES test_noise_source.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 600)

pulp_add_test_suite(pulp-test-decay-envelope
    SOURCES test_decay_envelope.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-lofi-chain
    SOURCES test_lofi_chain.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-lowpass-gate
    SOURCES test_lowpass_gate.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-two-pole-resonator
    SOURCES test_two_pole_resonator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Percussion voices. These cover the shared lifecycle (additive render, faded
# choke, velocity reaching timbre) alongside the voice's own behaviour, since
# a voice that broke a lifecycle rule would still sound plausible in isolation.
pulp_add_test_suite(pulp-test-drum-kick
    SOURCES test_drum_kick.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 600)

pulp_add_test_suite(pulp-test-drum-voices
    SOURCES test_drum_voices.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 600)

# The physically-modelled voices and their primitives. Each is measured against
# the thing a simpler construction cannot do: the shifter against a pitch shift,
# the string against a harmonic series it was never given, the membrane against
# the inharmonic ratios that stop it having a pitch.
pulp_add_test_suite(pulp-test-drum-physical
    SOURCES test_drum_physical.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

# The FM voices. The eight-operator routing table is data, so its suite runs
# every algorithm rather than a sample of them.
pulp_add_test_suite(pulp-test-drum-fm
    SOURCES test_drum_fm.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

# The Tier 0 mod-utilities toolkit: the shared modulation infrastructure the DSP
# series composes (planning/2026-07-25-dsp-series-round2.md, adjudication A-1).
# One executable keeps the toolkit's shared RT roster together, while focused
# sources keep the independent trigger and matrix families navigable.
pulp_add_test_suite(pulp-test-mod-utilities
    SOURCES test_mod_utilities.cpp
            test_mod_utilities_lfo.cpp
            test_mod_utilities_slew_envelope.cpp
            test_mod_utilities_vca_vactrol_trigger.cpp
            test_mod_utilities_trigger.cpp
            test_mod_utilities_mod_matrix.cpp
            harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 600)

# The saturation toolkit — the memoryless nonlinearity every drive/fuzz/tape/
# tube stage composes. The suite is the spec's acceptance set A1-A12; harmonic
# expectations are computed from the shipped closed forms, not restated.
pulp_add_test_suite(pulp-test-signal-saturator
    SOURCES test_signal_saturator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

# Feedforward compressor — the transparent/modern reference design, and the
# gain-computer core the VCA / FET / diode-bridge lineages compose. The suite is
# the spec's acceptance set 1-11; expected values are computed from the shipped
# constants and closed forms rather than restated.
pulp_add_test_suite(pulp-test-signal-feedforward-compressor
    SOURCES test_signal_feedforward_compressor.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

pulp_add_test_suite(pulp-test-dynamics-contract
    SOURCES test_dynamics_contract.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

# The circuit-modelled clipper family. Distinct from the memoryless saturator:
# a capacitor inside the clipping network makes the effective clip point a
# function of recent history, which is why these are ODEs solved per sample
# rather than a transfer-function table.
pulp_add_test_suite(pulp-test-signal-distortion
    SOURCES test_signal_distortion.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

# The two-transistor fuzz pair (M03). The clipping device IS the gain stage and
# the feedback path sets the operating point, so bias, gain and clipping shape
# are one interacting system — which is why the suite leans on the observables
# (stage gain, loop gain, solver residual) as much as on rendered audio.
pulp_add_test_suite(pulp-test-signal-fuzz-pair
    SOURCES test_signal_fuzz_pair.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

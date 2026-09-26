# DSP/MIDI realtime-contract test registrations kept out of the frozen
# top-level test manifest.

# Grouped executables (pulp_add_test_group in tools/cmake/PulpTestSuite.cmake):
# each member keeps its own registration, labels and timeout; only the binary
# behind them is shared. Almost every suite here links the RT allocation probe
# (harness/rt_allocation_probe.cpp), which replaces the global allocation
# operators for the whole process. That is why these suites cannot join a
# group of ordinary suites, but it does not stop them sharing a binary with
# each other: every member already ran under the same replaced operators, and
# the probe only counts inside an RtAllocationProbe scope on the calling
# thread. The groups therefore hold probe suites only, split by the compile
# line they already had: pulp::midi, pulp::format, pulp::signal, and
# pulp::signal with the FFT backend. A suite stays on its own when it has a
# compile line no sibling shares (parameter-event-queue on pulp::host,
# multi-channel-meter, reed-waveguide-loop, beat-repeat-kernel, modal-spec),
# does not link the probe (the FIR design suites, interaction-residual), is
# built with -fno-exceptions, or registers its target a second time with its
# own discovery call (modal-bank's bench cases).
pulp_add_test_group(pulp-test-group-dsp-rt-midi LIBRARIES pulp::midi)
pulp_add_test_group(pulp-test-group-dsp-rt-format LIBRARIES pulp::format)
pulp_add_test_group(pulp-test-group-dsp-rt-signal LIBRARIES pulp::signal)
pulp_add_test_group(pulp-test-group-dsp-rt-signal-fft
    LIBRARIES pulp::signal pulp::signal-fft-backend)

pulp_add_test_suite(pulp-test-sysex-accumulator GROUP pulp-test-group-dsp-rt-midi
    SOURCES test_sysex_accumulator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-ump-sysex7-reassembler GROUP pulp-test-group-dsp-rt-midi
    SOURCES test_ump_sysex7_reassembler.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-ump-packet-cursor GROUP pulp-test-group-dsp-rt-midi
    SOURCES test_ump_packet_cursor.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-processor-defaults GROUP pulp-test-group-dsp-rt-format
    SOURCES test_processor_defaults.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-raw-midi-parser GROUP pulp-test-group-dsp-rt-midi
    SOURCES test_raw_midi_parser.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-running-status GROUP pulp-test-group-dsp-rt-midi
    SOURCES test_running_status.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-parameter-event-queue
    SOURCES test_parameter_event_queue.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host)

pulp_add_test_suite(pulp-test-signal-rt-safety GROUP pulp-test-group-dsp-rt-signal-fft
    SOURCES test_signal_rt_safety.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::signal-fft-backend)

pulp_add_test_suite(pulp-test-expander GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_expander.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-auto-ducked-send GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_auto_ducked_send.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-source-filter-analysis GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_source_filter_analysis.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

pulp_add_test_suite(pulp-test-realtime-pitch-time-rt-safety GROUP pulp-test-group-dsp-rt-signal-fft
    SOURCES test_realtime_pitch_time_rt_safety.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::signal-fft-backend)

pulp_add_test_suite(pulp-test-signal-mod-rt-safety GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_mod_rt_safety.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

pulp_add_test_suite(pulp-test-multi-channel-meter
    SOURCES test_multi_channel_meter.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::audio)

pulp_add_test_suite(pulp-test-signal-sub-oscillator GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_sub_oscillator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

add_executable(pulp-test-signal-sub-oscillator-no-exceptions
    test_signal_sub_oscillator_no_exceptions.cpp)
target_link_libraries(pulp-test-signal-sub-oscillator-no-exceptions PRIVATE pulp::signal)
if(MSVC)
    target_compile_options(pulp-test-signal-sub-oscillator-no-exceptions PRIVATE /EHs-c- /GR-)
else()
    target_compile_options(pulp-test-signal-sub-oscillator-no-exceptions PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME signal-sub-oscillator-no-exceptions
    COMMAND pulp-test-signal-sub-oscillator-no-exceptions)

pulp_add_test_suite(pulp-test-signal-blit-oscillator GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_blit_oscillator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

add_executable(pulp-test-signal-blit-oscillator-no-exceptions
    test_signal_blit_oscillator_no_exceptions.cpp)
target_link_libraries(pulp-test-signal-blit-oscillator-no-exceptions PRIVATE pulp::signal)
if(MSVC)
    target_compile_options(pulp-test-signal-blit-oscillator-no-exceptions PRIVATE /EHs-c- /GR-)
else()
    target_compile_options(pulp-test-signal-blit-oscillator-no-exceptions PRIVATE -fno-exceptions -fno-rtti)
endif()
add_test(NAME signal-blit-oscillator-no-exceptions
    COMMAND pulp-test-signal-blit-oscillator-no-exceptions)

pulp_add_test_suite(pulp-test-midi-message-collector GROUP pulp-test-group-dsp-rt-midi
    SOURCES test_midi_message_collector.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-ump-buffer-conversion GROUP pulp-test-group-dsp-rt-format
    SOURCES test_ump_buffer_conversion.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-mpe-buffer GROUP pulp-test-group-dsp-rt-format
    SOURCES test_mpe_buffer.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-midi-subblock GROUP pulp-test-group-dsp-rt-format
    SOURCES test_midi_subblock.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)

pulp_add_test_suite(pulp-test-modal-bank
    SOURCES test_modal_bank.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TEST_SPEC "~[bench]"
    TIMEOUT 300)
pulp_scaled_test_timeout(_pulp_modal_bank_bench_timeout 300)
catch_discover_tests(pulp-test-modal-bank
    TEST_SPEC "[bench]"
    TEST_PREFIX "bench::"
    LABELS bench
    PROPERTIES TIMEOUT "${_pulp_modal_bank_bench_timeout}")

pulp_add_test_suite(pulp-test-bridged-t-resonator GROUP pulp-test-group-dsp-rt-signal
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
pulp_add_test_suite(pulp-test-square-osc-bank GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_square_osc_bank.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Percussion-synthesis shared layer. Each suite measures the property its
# component is named for -- spectral slope, decay timing, hold interval,
# vactrol asymmetry -- rather than asserting a coefficient against itself, so
# the naive transforms they use are the slow part and the timeouts are wide.
pulp_add_test_suite(pulp-test-noise-source GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_noise_source.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 600)

pulp_add_test_suite(pulp-test-stochastic-sources GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_stochastic_sources.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 600)

pulp_add_test_suite(pulp-test-decay-envelope GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_decay_envelope.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-lofi-chain GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_lofi_chain.cpp test_dither_velvet_noise.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-lowpass-gate GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_lowpass_gate.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-two-pole-resonator GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_two_pole_resonator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Percussion voices. These cover the shared lifecycle (additive render, faded
# choke, velocity reaching timbre) alongside the voice's own behaviour, since
# a voice that broke a lifecycle rule would still sound plausible in isolation.
pulp_add_test_suite(pulp-test-drum-kick GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_drum_kick.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 600)

pulp_add_test_suite(pulp-test-drum-voices GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_drum_voices.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 600)

# The physically-modelled voices and their primitives. Each is measured against
# the thing a simpler construction cannot do: the shifter against a pitch shift,
# the string against a harmonic series it was never given, the membrane against
# the inharmonic ratios that stop it having a pitch.
pulp_add_test_suite(pulp-test-drum-physical GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_drum_physical.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

# The FM voices. The eight-operator routing table is data, so its suite runs
# every algorithm rather than a sample of them.
pulp_add_test_suite(pulp-test-drum-fm GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_drum_fm.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

# The Tier 0 mod-utilities toolkit: the shared modulation infrastructure the DSP
# series composes (planning/2026-07-25-dsp-series-round2.md, adjudication A-1).
# One executable keeps the toolkit's shared RT roster together, while focused
# sources keep the independent trigger and matrix families navigable.
pulp_add_test_suite(pulp-test-mod-utilities GROUP pulp-test-group-dsp-rt-signal
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
pulp_add_test_suite(pulp-test-signal-saturator GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_saturator.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

pulp_add_test_suite(pulp-test-signal-nonlinear-shaping GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_nonlinear_shaping.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

pulp_add_test_suite(pulp-test-signal-parallel-dynamics GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_parallel_dynamics.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-signal-transfer-curve GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_transfer_curve.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-signal-cross-feedback-multitap-delay GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_cross_feedback_multitap_delay.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Fixed-capacity caller-authored early-reflection taps: independent sparse
# impulse oracle, true-stereo routing, headroom, partitioning, and RT proof.
pulp_add_test_suite(pulp-test-signal-early-reflections GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_early_reflections.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-signal-headphone-crossfeed GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_headphone_crossfeed.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-signal-diffusion-network GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_diffusion_network.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Feedforward compressor — the transparent/modern reference design, and the
# gain-computer core the VCA / FET / diode-bridge lineages compose. The suite is
# the spec's acceptance set 1-11; expected values are computed from the shipped
# constants and closed forms rather than restated.
pulp_add_test_suite(pulp-test-signal-feedforward-compressor GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_feedforward_compressor.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

pulp_add_test_suite(pulp-test-dynamics-contract GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_dynamics_contract.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

pulp_add_test_suite(pulp-test-de-esser GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_de_esser.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# The circuit-modelled clipper family. Distinct from the memoryless saturator:
# a capacitor inside the clipping network makes the effective clip point a
# function of recent history, which is why these are ODEs solved per sample
# rather than a transfer-function table.
pulp_add_test_suite(pulp-test-signal-distortion GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_distortion.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)

# The two-transistor fuzz pair (M03). The clipping device IS the gain stage and
# the feedback path sets the operating point, so bias, gain and clipping shape
# are one interacting system — which is why the suite leans on the observables
# (stage gain, loop gain, solver residual) as much as on rendered audio.
pulp_add_test_suite(pulp-test-signal-fuzz-pair GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_fuzz_pair.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 900)
# NLMS adaptive FIR: convergence, independent reference alignment, and the
# prepared audio-thread allocation contract live with the RT-owner suites.
pulp_add_test_suite(pulp-test-nlms-adaptive-filter GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_nlms_adaptive_filter.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

# Bounded digital-waveguide primitives: bidirectional prepared rails, passive
# reflection boundaries, and lossless fixed-capacity scattering junctions.
pulp_add_test_suite(pulp-test-waveguide-primitives GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_waveguide_primitives.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

# Public reed exciter and whole-loop owner: keep the allocation probe alongside
# the focused behavioral contract suite.
pulp_add_test_suite(pulp-test-reed-waveguide-loop
    SOURCES test_reed_waveguide_loop.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::audio-analysis)

# Tempo-map beat repeat: exact dry-history capture, frame-offset events, and
# the prepared allocation contract. Pitch remains a separately gated design.
pulp_add_test_suite(pulp-test-beat-repeat-kernel
    SOURCES test_beat_repeat_kernel.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::timebase)

# Depletion-aware stochastic collision and prepared voice contracts use
# independent statistical oracles plus the realtime allocation probe.
pulp_add_test_suite(pulp-test-particle-percussion GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_particle_percussion.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Offline FIR design from sampled targets. The suite covers all four real
# linear-phase symmetry forms, independent dense response/weighted-residual
# oracles, planted weight/normalization mutations, and the cepstral
# minimum-phase reconstruction. It is not realtime DSP.
pulp_add_test_suite(pulp-test-signal-fir-design
    SOURCES test_signal_fir_design.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Equiripple FIR design by the Remez exchange. The alternation theorem is the
# in-test oracle: r independent coefficients must yield r+1 alternating extrema
# of equal weighted magnitude, checked from the returned taps by a direct DFT.
# The least-squares twin at the same order is the negative control. Offline
# design work, not realtime DSP.
pulp_add_test_suite(pulp-test-signal-fir-remez
    SOURCES test_signal_fir_remez.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

# Prepared streaming reverse windows: independent hand-sequence oracle,
# boundary fading, latency/tail/state, partitioning, and RT proof.
pulp_add_test_suite(pulp-test-signal-reverse-buffer GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_signal_reverse_buffer.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

pulp_add_test_suite(pulp-test-tempo-delay GROUP pulp-test-group-dsp-rt-signal
    SOURCES test_tempo_delay.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal
    TIMEOUT 300)

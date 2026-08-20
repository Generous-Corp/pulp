# DSP series round 2 — per-module test registration.
#
# One glob-free registration per module, so a module's suite is owned
# entirely by that module's own files. This exists because the series is built
# by several people (or agents) working in parallel on disjoint modules: a
# single shared registration file is the one place they would all have to edit,
# and therefore the one place they would conflict.
#
# Every listed suite is required. A missing source is a configuration error: the
# test tree must fail closed rather than silently publishing less coverage.
#
# Adding a module: append a block. Nothing else in the build system changes.

function(pulp_dsp_series_signal_suite name)
    set(sources ${ARGN})
    foreach(source IN LISTS sources)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
            message(FATAL_ERROR "Missing required DSP series test source: ${source}")
        endif()
    endforeach()
    pulp_add_test_suite(${name}
        SOURCES ${sources} harness/rt_allocation_probe.cpp
        LIBRARIES pulp::signal
        TIMEOUT 900)
endfunction()

function(pulp_dsp_series_catalog_suite name)
    set(sources ${ARGN})
    foreach(source IN LISTS sources)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
            message(FATAL_ERROR "Missing required DSP series test source: ${source}")
        endif()
    endforeach()
    add_executable(${name} ${sources})
    target_sources(${name} PRIVATE
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
        $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
    target_link_libraries(${name} PRIVATE pulp::host pulp::signal Catch2::Catch2WithMain)
    catch_discover_tests(${name})
endfunction()

pulp_dsp_series_signal_suite(pulp-test-signal-analysis-frontends
                             test_signal_analysis_frontends.cpp)
pulp_dsp_series_signal_suite(pulp-test-spectral-feature-frontends
                             test_spectral_feature_frontends.cpp)

# Each extracted production header is compiled in its own translation unit.
# This catches accidental reliance on an umbrella header's include order while
# keeping the public umbrella APIs unchanged.
add_library(pulp-dsp-series-header-self-containment OBJECT
    header_compile/early_reflections.cpp
    header_compile/additive_spectral_envelope.cpp
    header_compile/auto_ducked_send.cpp
    header_compile/comb_filter.cpp
    header_compile/cartesian_walk.cpp
    header_compile/dynamics_contract.cpp
    header_compile/dynamic_eq.cpp
    header_compile/fast_math.cpp
    header_compile/formant_filter_bank.cpp
    header_compile/graphic_eq.cpp
    header_compile/gate_logic.cpp
    header_compile/leslie_rotary.cpp
    header_compile/probability_gate.cpp
    header_compile/rungler.cpp
    header_compile/scale_quantizer.cpp
    header_compile/scanner_vibrato.cpp
    header_compile/spectral_cross_synthesis.cpp
    header_compile/stage_sequencer.cpp
    header_compile/transport_edge.cpp
    header_compile/yin_tracker.cpp
    header_compile/tape_machine_components.cpp
    header_compile/nonlin_ambience_design.cpp
    header_compile/parallel_dynamics.cpp
    header_compile/transfer_curve.cpp
    header_compile/explicit_q_resonator_bank.cpp
    header_compile/beat_repeat_kernel.cpp
    header_compile/transient_designer.cpp
    header_compile/spectral_mask_processor.cpp
    header_compile/spectral_delay_matrix.cpp
    header_compile/tempo_delay.cpp
    header_compile/zero_latency_convolver_support.cpp)
target_sources(pulp-dsp-series-header-self-containment PRIVATE
    header_compile/filter_morph.cpp)
target_link_libraries(pulp-dsp-series-header-self-containment PRIVATE pulp::signal)

pulp_dsp_series_signal_suite(pulp-test-signal-fractional-delay
                             test_signal_fractional_delay.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-filter-morph
                             test_signal_filter_morph.cpp)

pulp_dsp_series_signal_suite(pulp-test-signal-explicit-q-resonator-bank
                             test_explicit_q_resonator_bank.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-spectral-delay-matrix
                             test_spectral_delay_matrix.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-comb-filter
                             test_signal_comb_filter.cpp)

# ── The modules ───────────────────────────────────────────────────────────
pulp_dsp_series_signal_suite(pulp-test-signal-tape-machine   test_signal_tape_machine_eq_nonlinearity_archetypes.cpp
    test_signal_tape_machine_latency_rt_faults.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-tape-catalog   test_forge_tape_catalog.cpp)

pulp_dsp_series_signal_suite(pulp-test-signal-vca-compressor test_signal_vca_compressor.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-transient-designer
                             test_signal_transient_designer.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-dynamic-eq test_signal_dynamic_eq.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-formant-filter-bank
                             test_signal_formant_filter_bank.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-fet-compressor test_signal_fet_compressor_curve_ballistics_colour.cpp
    test_signal_fet_compressor_controls_rt.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-diode-bridge-compressor
                             test_signal_diode_bridge_compressor_curve_ballistics.cpp
    test_signal_diode_bridge_compressor_transformer_rt.cpp)
# The three compressor lineages (VCA / FET / diode-bridge) ship their DSP blocks
# here but NOT their own catalog headers: they are realizations of the compressor
# family, so their nodes join forge_dynamics_catalog.hpp alongside the
# feedforward design they all compose. One family, one catalog header — see that
# file's own note on why it is named for the family rather than the member.
pulp_dsp_series_catalog_suite(pulp-test-forge-dynamics-lineage-catalog
                              test_forge_dynamics_lineage_catalog.cpp)

# ── Modulation and pitch/time ──────────────────────────────────────────────
pulp_dsp_series_signal_suite(pulp-test-signal-frequency-shifter-ssb
                             test_signal_frequency_shifter_ssb.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-chorus-family  test_signal_chorus_family_voicings_colour.cpp
    test_signal_chorus_family_rt_contracts.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-flanger        test_signal_flanger_comb_through_zero.cpp
    test_signal_flanger_barberpole_bbd.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-phaser-stages  test_signal_phaser_stages_response_sweep.cpp
    test_signal_phaser_stages_rt_contracts.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-vibrato        test_signal_vibrato_delay_phase_univibe.cpp
    test_signal_vibrato_rt_params_safety.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-leslie
    test_signal_leslie_rotary.cpp
    test_signal_leslie_scanner.cpp
    test_signal_leslie_contracts.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-pitch-shifter  test_signal_pitch_shifter_core_pedal.cpp
    test_signal_pitch_shifter_interpolation_gain.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-harmony-engine test_signal_harmony_engine_tracking_mapping_audio.cpp
    test_signal_harmony_engine_rt_contracts.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-cyclic-stretch test_signal_cyclic_stretch.cpp)

# ── Synthesis and spectral ─────────────────────────────────────────────────
pulp_dsp_series_signal_suite(pulp-test-signal-granular       test_signal_granular_window_schedule_gain.cpp
    test_signal_granular_live_rt_safety.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-additive-bank  test_signal_additive_bank_spectrum_envelopes.cpp
    test_signal_additive_bank_gain_rt_voices.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-fm-operator-engine test_signal_fm_operator_engine.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-vocoder        test_signal_vocoder_filter_voicing_formant.cpp
    test_signal_vocoder_rt_composition_safety.cpp)

# ── Space and physical modelling ───────────────────────────────────────────
pulp_dsp_series_signal_suite(pulp-test-signal-zero-latency-convolver
                             test_signal_zero_latency_convolver_schedule_quality.cpp
    test_signal_zero_latency_convolver_ingest_rt.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-speaker-cabinet test_signal_speaker_cabinet_physics_gain.cpp
    test_signal_speaker_cabinet_rt_geometry.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-nonlin-ambience test_signal_nonlin_ambience_topology_render.cpp
    test_signal_nonlin_ambience_rt_quality.cpp)

# ── CV / sequencing ────────────────────────────────────────────────────────
pulp_dsp_series_signal_suite(pulp-test-signal-modular-sequencing
    test_signal_modular_sequencing_reset_transport.cpp
    test_signal_modular_sequencing_stage_cartesian_rungler.cpp
    test_signal_modular_sequencing_quantizer_gates.cpp
    test_signal_modular_sequencing_contracts.cpp)

# ── Catalog suites (one per module that ships its own catalog header) ──────
pulp_dsp_series_catalog_suite(pulp-test-forge-effect-modulation-catalog
                              test_forge_effect_modulation_catalog_frequency_chorus_phaser.cpp
    test_forge_effect_modulation_catalog_vibrato_flanger_leslie.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-pitch-catalog  test_forge_pitch_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-synthesis-catalog
                              test_forge_synthesis_catalog_contracts.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-space-catalog  test_forge_space_catalog_convolution_ambience_topology.cpp
    test_forge_space_catalog_ambience_runtime_speaker.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-sequencing-catalog
                              test_forge_sequencing_catalog_stage_cartesian_rungler.cpp
    test_forge_sequencing_catalog_quantizer_gates_contracts.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-character-delay-catalog
                              test_forge_character_delay_catalog.cpp)

# The three catalog suites written before this file existed, plus the fixture
# smoke. They were hand-registered in app_audio_host_tests.cmake; they belong
# here with the rest of the series.
#
# Moving them is not only tidiness. That file is one of the hashed inputs in
# `verify_sampler_interpolation_benchmark.py`'s source bundle, which exists so a
# recorded benchmark result cannot outlive a change to the code it measured.
# Appending unrelated registrations there invalidated the recorded evidence and
# failed the check — correctly. The series owns its own registration file
# precisely so it never perturbs someone else's provenance.
pulp_dsp_series_catalog_suite(pulp-test-forge-saturator-catalog
                              test_forge_saturator_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-fuzz-catalog
                              test_forge_fuzz_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-dynamics-catalog
                              test_forge_dynamics_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-distortion-catalog
                              test_forge_distortion_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-spectral-dynamics-catalogs
                              test_forge_spectral_dynamics_catalogs.cpp)

# Smoke test for the shared bake-layer fixture. Small on purpose: its job is to
# keep the fixture compiling against both a mono and a stereo catalog node, so a
# change to either shape fails here rather than in whichever suite happens to be
# rebuilt next.
pulp_dsp_series_catalog_suite(pulp-test-baked-node-fixture
                              test_baked_node_fixture_smoke.cpp)

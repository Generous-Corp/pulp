# DSP series round 2 — per-module test registration.
#
# One glob-free, self-activating block per module, so a module's suite is owned
# entirely by that module's own files. This exists because the series is built
# by several people (or agents) working in parallel on disjoint modules: a
# single shared registration file is the one place they would all have to edit,
# and therefore the one place they would conflict.
#
# Each block guards on its source existing, so a module whose suite is written
# but not yet ready simply does not register — no red tree, and nothing hidden,
# because the guard is visible right here.
#
# Adding a module: append a block. Nothing else in the build system changes.

function(pulp_dsp_series_signal_suite name source)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        pulp_add_test_suite(${name}
            SOURCES ${source} harness/rt_allocation_probe.cpp
            LIBRARIES pulp::signal
            TIMEOUT 900)
    endif()
endfunction()

function(pulp_dsp_series_catalog_suite name source)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        add_executable(${name} ${source})
        target_sources(${name} PRIVATE
            $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
            $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
        target_link_libraries(${name} PRIVATE pulp::host pulp::signal Catch2::Catch2WithMain)
        catch_discover_tests(${name})
    endif()
endfunction()

# ── The modules ───────────────────────────────────────────────────────────
pulp_dsp_series_signal_suite(pulp-test-signal-tape-machine   test_signal_tape_machine.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-tape-catalog   test_forge_tape_catalog.cpp)

pulp_dsp_series_signal_suite(pulp-test-signal-vca-compressor test_signal_vca_compressor.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-fet-compressor test_signal_fet_compressor.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-diode-bridge-compressor
                             test_signal_diode_bridge_compressor.cpp)
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
pulp_dsp_series_signal_suite(pulp-test-signal-chorus-family  test_signal_chorus_family.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-flanger        test_signal_flanger.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-phaser-stages  test_signal_phaser_stages.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-vibrato        test_signal_vibrato.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-leslie         test_signal_leslie.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-pitch-shifter  test_signal_pitch_shifter.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-harmony-engine test_signal_harmony_engine.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-cyclic-stretch test_signal_cyclic_stretch.cpp)

# ── Synthesis and spectral ─────────────────────────────────────────────────
pulp_dsp_series_signal_suite(pulp-test-signal-granular       test_signal_granular.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-additive-bank  test_signal_additive_bank.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-vocoder        test_signal_vocoder.cpp)

# ── Space and physical modelling ───────────────────────────────────────────
pulp_dsp_series_signal_suite(pulp-test-signal-zero-latency-convolver
                             test_signal_zero_latency_convolver.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-speaker-cabinet test_signal_speaker_cabinet.cpp)
pulp_dsp_series_signal_suite(pulp-test-signal-nonlin-ambience test_signal_nonlin_ambience.cpp)

# ── CV / sequencing ────────────────────────────────────────────────────────
pulp_dsp_series_signal_suite(pulp-test-signal-modular-sequencing
                             test_signal_modular_sequencing.cpp)

# ── Catalog suites (one per module that ships its own catalog header) ──────
pulp_dsp_series_catalog_suite(pulp-test-forge-modulation-catalog
                              test_forge_modulation_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-pitch-catalog  test_forge_pitch_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-synthesis-catalog
                              test_forge_synthesis_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-space-catalog  test_forge_space_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-sequencing-catalog
                              test_forge_sequencing_catalog.cpp)
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
pulp_dsp_series_catalog_suite(pulp-test-forge-dynamics-catalog
                              test_forge_dynamics_catalog.cpp)
pulp_dsp_series_catalog_suite(pulp-test-forge-distortion-catalog
                              test_forge_distortion_catalog.cpp)

# Smoke test for the shared bake-layer fixture. Small on purpose: its job is to
# keep the fixture compiling against both a mono and a stereo catalog node, so a
# change to either shape fails here rather than in whichever suite happens to be
# rebuilt next.
pulp_dsp_series_catalog_suite(pulp-test-baked-node-fixture
                              test_baked_node_fixture_smoke.cpp)

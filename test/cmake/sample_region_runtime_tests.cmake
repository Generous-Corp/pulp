# Focused sample-region runtime acceptance. Keep this target independently
# selectable: R2 validation runs its acceptance families by Catch tag instead
# of sweeping the broader graph/audio suites.
pulp_add_test_suite(pulp-test-sample-region-runtime
    SOURCES
        test_sample_region_runtime.cpp
        harness/rt_allocation_probe.cpp
        support/audio_signal_generators.cpp
    LIBRARIES
        pulp::host
        pulp::format
        pulp::graph
        pulp::audio
        pulp::audio-analysis)

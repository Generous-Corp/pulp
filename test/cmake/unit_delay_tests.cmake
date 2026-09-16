# Public one-sample state helper: independent scalar and harness impulse/reset
# oracles, causal ordering negative control, and realtime allocation probe.
pulp_add_test_suite(pulp-test-unit-delay
    SOURCES
        test_unit_delay.cpp
        harness/rt_allocation_probe.cpp
        support/audio_signal_generators.cpp
        support/render_scenario.cpp
    LIBRARIES pulp::format pulp::audio-analysis pulp::signal)

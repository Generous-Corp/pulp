# S4 focused proof: Timeline automation and graph modulation both reach
# authored ProcessorNode instances through the ordinary graph-owned routes.
pulp_add_test_suite(pulp-test-sample-region-sequencer
    SOURCES
        test_sample_region_sequencer.cpp
    LIBRARIES
        pulp::host
        pulp::format
        pulp::graph
        pulp::audio
        sample-region-allpass-core
        pulp::timeline
    COMPILE_DEFINITIONS
        PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

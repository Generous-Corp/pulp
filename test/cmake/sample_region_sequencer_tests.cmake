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

# The authority-boundary case reads its exposure row, the example's README and
# the two host sources whose behaviour it pins.
pulp_test_data(pulp-test-sample-region-sequencer NO_DEFINE PATHS
    docs/status/sequencer-exposure/rows/sample-region-sequencer-interoperability.json
    examples/sample-region-allpass/README.md
    core/host/src/timeline_automation_delivery.cpp
    core/host/src/signal_graph.cpp)

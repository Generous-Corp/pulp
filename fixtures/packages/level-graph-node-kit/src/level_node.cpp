// Fixture source path for a source-built SignalGraph custom node kit.
// The actual runtime behavior is covered by test_host_signal_graph.cpp.
namespace pulp_fixture_level_node {
float process_sample(float input, float level) {
    return input * level;
}
}

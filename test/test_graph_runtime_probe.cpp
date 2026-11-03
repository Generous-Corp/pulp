#include <pulp/graph/graph_runtime_probe.hpp>

#include <cassert>

int main() {
    pulp::graph::GraphRuntimeProbe<2> probe;
    pulp::graph::GraphRuntimeProbeSample sample{};
    assert(!probe.try_push({}));
    probe.attach();
    assert(probe.try_push(
        {.timestamp_samples = 64, .edge_index = 3, .frames = 64, .peak = 0.5f, .rms = 0.25f}));
    assert(probe.try_pop(sample));
    assert(sample.sequence == 0 && sample.edge_index == 3 && sample.frames == 64);
    assert(!probe.try_pop(sample));
    assert(probe.try_push({}));
    assert(probe.try_push({}));
    assert(!probe.try_push({}));
    assert(probe.dropped() == 1);
    probe.detach();
    assert(!probe.try_push({}));
}

#include <pulp/host/signal_graph_runtime.hpp>

#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
struct DelayState {
    float history[4]{};
    std::uint32_t cursor = 0;
};

pulp::host::CustomNodeType delay_type() {
    pulp::host::CustomNodeType type;
    type.type_id = "pulp.g7.consumer.delay4";
    type.version = 1;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.default_name = "Delay4";
    type.latency_samples = [](double) { return 4; };
    type.create = [] { return static_cast<void*>(new DelayState{}); };
    type.destroy = [](void* value) { delete static_cast<DelayState*>(value); };
    type.reset = [](void* value) { *static_cast<DelayState*>(value) = {}; };
    type.process_instance = [](void* value, pulp::audio::BufferView<float>& out,
                               const pulp::audio::BufferView<const float>& in, int frames) {
        auto& state = *static_cast<DelayState*>(value);
        for (int i = 0; i < frames; ++i) {
            out.channel_ptr(0)[i] = state.history[state.cursor];
            state.history[state.cursor] = in.channel_ptr(0)[i];
            state.cursor = (state.cursor + 1) % 4;
        }
    };
    return type;
}

void require(bool condition, const char* rule) {
    if (!condition)
        throw std::runtime_error(rule);
}
} // namespace

int main() try {
    pulp::host::SignalGraph graph;
    require(graph.register_custom_node_type(delay_type()), "register-delay");
    const auto input = graph.add_input_node(1, "input");
    const auto delayed = graph.add_custom_node("pulp.g7.consumer.delay4", "delayed");
    const auto dry = graph.add_gain_node("dry");
    const auto output = graph.add_output_node(1, "output");
    require(input != 0 && delayed != 0 && dry != 0 && output != 0, "nodes");
    require(graph.connect(input, 0, delayed, 0), "delayed-edge");
    require(graph.connect(delayed, 0, output, 0), "delayed-output");
    require(graph.connect(input, 0, dry, 0), "dry-edge");
    require(graph.connect(dry, 0, output, 0), "dry-output");
    require(graph.prepare(48000.0, 64), "prepare");
    require(graph.latency_samples() == 4, "total-pdc");
    // This query reports latency arriving at the node input; the intrinsic
    // four samples are reflected in the externally reported total.
    require(graph.node_latency_samples(delayed) == 0, "node-input-latency");

    const auto latency = graph.prepared_latency_snapshot();
    require(latency.has_value(), "latency-snapshot");
    require(latency->total_latency_samples == 4, "snapshot-total-pdc");
    const auto delayed_value = latency->node(delayed);
    const auto dry_value = latency->node(dry);
    const auto output_value = latency->node(output);
    require(delayed_value.has_value() && delayed_value->input_latency_samples == 0 &&
                delayed_value->output_latency_samples == 4,
            "delayed-node-input-output");
    require(dry_value.has_value() && dry_value->input_latency_samples == 0 &&
                dry_value->output_latency_samples == 0,
            "dry-node-input-output");
    require(output_value.has_value() && output_value->input_latency_samples == 4 &&
                output_value->output_latency_samples == 4,
            "output-node-input-output");

    const pulp::host::Connection delayed_edge{delayed, 0, output, 0};
    const pulp::host::Connection dry_edge{dry, 0, output, 0};
    require(latency->connection_delay(delayed_edge) == 0, "delayed-branch-no-pdc");
    require(latency->connection_delay(dry_edge) == 4, "dry-branch-pdc");

    // Planted connection negative: endpoint identities absent from the
    // prepared snapshot never produce a fabricated zero-delay answer.
    require(!latency->connection_delay(pulp::host::Connection{999999, 0, output, 0}).has_value(),
            "unknown-connection-nullopt");

    // Planted negative: an unknown node never yields a fabricated latency entry.
    require(graph.node_latency_samples(999999) == 0, "unknown-node-zero");
    require(!latency->node(999999).has_value(), "unknown-node-nullopt");

    // A copied map remains stable, while a topology mutation retires the
    // published prepared snapshot until the next prepare().
    require(graph.disconnect(input, 0, dry, 0), "disconnect-for-mutation");
    require(!graph.prepared_latency_snapshot().has_value(),
            "mutation-invalidates-prepared-snapshot");
    require(latency->total_latency_samples == 4 && latency->connections.size() == 4,
            "copied-snapshot-stable-after-mutation");
    require(graph.prepare(48000.0, 64), "reprepare-after-mutation");
    require(graph.prepared_latency_snapshot().has_value(), "reprepared-latency-snapshot");
    std::cout << "g7-latency-pdc PASS total_latency_samples=" << graph.latency_samples()
              << " delayed_node_input_latency_samples=" << graph.node_latency_samples(delayed)
              << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "g7-latency-pdc FAIL rule=" << error.what() << '\n';
    return 1;
}

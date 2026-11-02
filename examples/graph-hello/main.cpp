#include <pulp/audio/buffer.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <cmath>
#include <iostream>
#include <vector>

int main() {
    using namespace pulp::host;

    SignalGraph graph;
    const auto input = graph.add_input_node(1, "hello-input");
    const auto gain = graph.add_gain_node("hello-gain");
    const auto output = graph.add_output_node(1, "hello-output");
    if (!input || !gain || !output || !graph.connect(input, 0, gain, 0) ||
        !graph.connect(gain, 0, output, 0) || !graph.set_node_gain(gain, 0.5f)) {
        std::cerr << "graph authoring failed\n";
        return 2;
    }

    graph.set_limits({.max_nodes = 3,
                      .max_connections = 2,
                      .max_ports = 6,
                      .max_block_size = 64,
                      .max_estimated_work_units = 4096});
    if (!graph.validate_generated_graph(16).accepted) {
        std::cerr << "graph validation failed\n";
        return 3;
    }

    OfflineSignalGraphHost host(graph);
    if (!host.prepare({.sample_rate = 48000.0,
                       .block_frames = 16,
                       .input_channels = 1,
                       .output_channels = 1})) {
        std::cerr << "offline prepare failed\n";
        return 4;
    }

    std::vector<float> input_samples(32, 1.0f);
    float* input_ptrs[] = {input_samples.data()};
    pulp::audio::BufferView<const float> input_view(input_ptrs, 1, input_samples.size());
    const auto rendered = host.render({.frame_count = input_samples.size(), .input = input_view});
    if (!rendered.ok || rendered.audio.num_channels() != 1 || rendered.audio.num_samples() != 32) {
        std::cerr << "offline render failed\n";
        return 5;
    }
    for (const float sample : rendered.audio.channel(0)) {
        if (std::abs(sample - 0.5f) > 1.0e-6f) {
            std::cerr << "unexpected rendered sample: " << sample << '\n';
            return 6;
        }
    }
    std::cout << "graph-hello ok frames=" << rendered.audio.num_samples()
              << " blocks=" << rendered.blocks_rendered << '\n';
    return 0;
}

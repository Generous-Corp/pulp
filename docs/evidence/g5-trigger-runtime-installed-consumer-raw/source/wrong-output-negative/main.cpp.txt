#include <array>
#include <iostream>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/trigger_graph.hpp>
#include <vector>

int main() {
    using namespace pulp::host;
    SignalGraph graph;
    auto input = graph.add_input_node(2);
    auto output = graph.add_output_node(1);
    TriggerProducerConfig config;
    config.lane_count = 2;
    config.lanes[0].payload.token = 7;
    config.lanes[1].payload.token = 9;
    auto producer = graph.add_trigger_producer(config);
    auto envelope = graph.add_trigger_envelope({0.5f});
    auto modulate = graph.add_trigger_modulation();
    auto lowpass = graph.add_trigger_lowpass(0.5f);
    if (!graph.connect(input, 0, producer, 0) || !graph.connect(input, 1, modulate, 1) ||
        !graph.connect(envelope, 0, modulate, 0) || !graph.connect(modulate, 0, lowpass, 0) ||
        !graph.connect(lowpass, 0, output, 0) ||
        graph.connect_trigger({producer, envelope}) != TriggerGraphResult::Accepted)
        return 1;
    auto candidate = graph.prepare_trigger_plan(48000, 8);
    if (candidate.result != TriggerGraphResult::Accepted) {
        std::cerr << "prepare refused " << static_cast<int>(candidate.result) << '\n';
        return 1;
    }
    if (graph.try_install_prepared_trigger_plan(candidate.plan) != TriggerGraphResult::Accepted)
        return 1;
    std::array<float, 8> source{0, 1, 1, 0, 1, 0, 0, 0}, result{};
    std::array<float, 8> carrier;
    carrier.fill(0.25f);
    std::array<const float*, 2> in_ptr{source.data(), carrier.data()};
    float* out_ptr = result.data();
    pulp::audio::BufferView<const float> in(in_ptr.data(), 2, source.size());
    pulp::audio::BufferView<float> out(&out_ptr, 1, result.size());
    if (graph.process_checked(out, in, 8) != TriggerGraphResult::Accepted)
        return 1;
    constexpr std::array<float, 8> expected{0,         0.125f,     0.25f,      0.09375f,
                                            0.171875f, 0.1484375f, 0.10546875f, 0.068359375f};
    if (result != expected) {
        for (auto v : result)
            std::cerr << v << ' ';
        return 1;
    }
    const auto trace = graph.trigger_trace();
    if (trace.size() != 4)
        return 1;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        if (trace[i].sample != (i < 2 ? 1u : 4u) || trace[i].sequence != i ||
            trace[i].token != (i % 2 == 0 ? 7 : 9) || trace[i].epoch != 1 ||
            trace[i].producer != producer || trace[i].source_port != 0 ||
            trace[i].kind != TriggerKind::Trigger)
            return 1;
    }
    if (graph.try_reset_events() != TriggerGraphResult::Accepted ||
        graph.process_checked(out, in, 8) != TriggerGraphResult::Accepted || result != expected)
        return 1;
    std::cout << "PASS: typed onset, ties, envelope, modulation and lowpass audio\n";
}

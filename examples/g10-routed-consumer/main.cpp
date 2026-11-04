#include <pulp/host/signal_graph.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

auto render(int partition) {
    using namespace pulp::host;
    const auto shape = *SpectralIdentity::make(256, 63, 1, 48000).value;
    SignalGraph graph;
    const auto in = graph.add_input_node(1);
    const auto out = graph.add_output_node(2);
    const auto analysis = *graph.add_spectral_analysis(shape, "analysis").value;
    const auto first = *graph.add_spectral_stage(shape,
        *QualifiedSpectralStage::identity_v1(shape).value, "identity").value;
    const auto second = *graph.add_spectral_stage(shape,
        *QualifiedSpectralStage::real_mask_v1(shape, std::vector<float>(129, 0.5f)).value,
        "mask").value;
    const auto synthesis = *graph.add_spectral_synthesis(shape, "wet").value;
    const auto sibling = *graph.add_spectral_synthesis(shape, "sibling").value;
    auto connect = [&](auto a, auto b) { require(bool(graph.connect(a, b)), "named connection"); };
    connect(graph.node(in)->out("audio"), analysis.audio[0]);
    connect(analysis.frames, first.frames_in);
    connect(first.frames_out, second.frames_in);
    connect(second.frames_out, synthesis.frames);
    connect(first.frames_out, sibling.frames);
    connect(synthesis.audio[0], graph.node(out)->in("audio"));
    connect(graph.node(in)->out("audio"), graph.node(out)->in("audio"));
    require(graph.connect(sibling.node, 0, out, 1), "sibling audio route");
    require(!graph.connect(analysis.frames, second.frames_in), "join must reject");
    require(!graph.connect(synthesis.node, 0, analysis.node, 0), "cycle must reject");
    require(!graph.connect(analysis.node, 0, first.node, 0), "untyped frame edge must reject");
    require(!QualifiedSpectralStage::real_mask_v1(shape, std::vector<float>(129, 2)), "mask range");
    require(!SpectralIdentity::make(256, 129, 1, 48000), "hop bound");
    graph.set_parallel_routing_enabled(true);
    graph.set_anticipation_enabled(true);
    require(graph.prepare(48000, 4096), "prepare");
    require(graph.latency_samples() == 319, "N+H latency");
    graph.set_canonical_executor_routing_enabled(false);
    constexpr int total = 8192, length = 6000;
    std::vector<float> input(total), wet(total), other(total);
    for (int i = 0; i < length; ++i) input[i] = 0.2f * std::sin(i * 0.117f);
    input[0] = input[5011] = 1;
    for (int pos = 0; pos < total;) {
        const int count = std::min(total - pos, partition ? partition : 1 + pos * 17 % 4096);
        const float* ip[] = {input.data() + pos};
        float* op[] = {wet.data() + pos, other.data() + pos};
        pulp::audio::BufferView<const float> iv(ip, 1, count);
        pulp::audio::BufferView<float> ov(op, 2, count);
        graph.process(ov, iv, count);
        pos += count;
    }
    double energy = 0, error = 0;
    for (int i = 0; i < total; ++i) {
        const float wanted = i >= 319 ? input[i - 319] : 0;
        const double delta = wet[i] - 1.5 * wanted;
        energy += wanted * wanted;
        error += delta * delta;
        require(std::abs(other[i] - wanted) < 1e-3f, "independent fanout/PDC");
        if (i < 319 || i >= length + 319 + 255)
            require(wet[i] == 0 && other[i] == 0, "prefix/tail silence");
    }
    require(error / energy < 1e-10, "reconstruction below -100 dB");
    require(graph.routed_walk_fallbacks() == 0, "no legacy fallback");
    return wet;
}

int main() {
    try {
        const auto fixed = render(64);
        require(render(4096) == fixed, "large partition invariance");
        require(render(0) == fixed, "irregular partition invariance");
        std::cout << "G-10 routed consumer: positive and negative controls passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

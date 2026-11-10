#include "asset_morph_oracle.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <pulp/host/signal_graph.hpp>

int main(int argc, char** argv) {
    try {
        using namespace pulp::host;
        using namespace asset_oracle;
        const auto a = analyze(stimulus(0)), b = analyze(stimulus(1));
        auto bytes_a = encode(a), bytes_b = encode(b);
        std::vector<float> control(a.size() * 64 + 1024);
        for (std::size_t i = 0; i < control.size(); ++i)
            control[i] = float((i * 13 % 257) / 256.0);
        const auto wanted = expected(a, b, control);
        std::vector<float> reference;
        for (int partition : {32, 64, 128, 0}) {
            SignalGraph graph;
            const auto input = graph.add_input_node(1), output = graph.add_output_node(1);
            auto source =
                graph.add_spectral_asset_morph_v1(bytes_a.view(), bytes_b.view(), 769, "assets");
            require(bool(source), "asset factory");
            const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
            const auto synthesis = *graph.add_spectral_synthesis(shape, "synthesis").value;
            require(bool(graph.connect(graph.node(input)->out("audio"), source.value->morph)),
                    "live control");
            require(bool(graph.connect(source.value->frames, synthesis.frames)), "typed frames");
            require(bool(graph.connect(synthesis.audio[0], graph.node(output)->in("audio"))),
                    "audio output");
            require(graph.prepare(48000, 4096), "prepare");
            require(graph.latency_samples() == 320, "latency");
            std::vector<float> audio(control.size());
            for (int t = 0; t < int(control.size());) {
                const int count =
                    std::min(int(control.size()) - t, partition ? partition : 1 + t * 17 % 511);
                const float* ip[] = {control.data() + t};
                float* op[] = {audio.data() + t};
                pulp::audio::BufferView<const float> iv(ip, 1, count);
                pulp::audio::BufferView<float> ov(op, 1, count);
                graph.process(ov, iv, count);
                t += count;
            }
            compare(audio, wanted);
            require(reference.empty() || reference == audio, "partition invariance");
            reference = audio;
            for (std::size_t i = 0; i < audio.size(); ++i)
                if (i < 320 || i >= source.value->render_frames)
                    require(audio[i] == 0, "prefix and terminal silence");
            for (const auto& d : graph.poll_live_dsp_telemetry().spectral)
                require(d.first_error == 0, "spectral diagnostics");
            require(graph.routed_walk_fallbacks() == 0, "serial executor");
        }
        if (argc == 2) {
            std::filesystem::create_directories(argv[1]);
            auto write = [&](const char* name, const void* data, std::size_t size) {
                std::ofstream out(std::filesystem::path(argv[1]) / name, std::ios::binary);
                out.write(static_cast<const char*>(data), size);
                require(bool(out), "artifact write");
            };
            write("asset-a.sfrm", bytes_a.bytes.get(), bytes_a.size);
            write("asset-b.sfrm", bytes_b.bytes.get(), bytes_b.size);
            write("control.f32", control.data(), control.size() * sizeof(float));
            write("output.f32", reference.data(), reference.size() * sizeof(float));
            write("expected.f64", wanted.data(), wanted.size() * sizeof(double));
        }
        std::cout << "Fused asset morph: direct DFT oracle and live partition controls passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

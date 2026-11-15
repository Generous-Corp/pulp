#include "../g10-routed-consumer/asset_morph_oracle.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/runtime/crypto.hpp>
#include <sstream>

namespace fs = std::filesystem;
using namespace pulp::host;
using namespace asset_oracle;
using Bytes = std::vector<std::uint8_t>;
Bytes read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    require(bool(in), "missing saved input");
    const auto size = fs::file_size(path);
    require(size <= GraphSerializer::max_file_bytes, "saved input bound");
    Bytes result(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(size));
    require(bool(in), "saved input read");
    return result;
}
std::string hash(const Bytes& bytes) {
    const auto digest = pulp::runtime::sha256(bytes.data(), bytes.size());
    const char* digits = "0123456789abcdef";
    std::string out;
    for (auto b : digest) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}
void write(const fs::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    require(bool(out), "saved input write");
}
Bytes text_bytes(const std::string& s) {
    return {s.begin(), s.end()};
}
std::vector<float> controls() {
    std::vector<float> out(16 * 64 + 1024);
    for (std::size_t i = 0; i < out.size(); ++i)
        out[i] = float((i * 13 % 257) / 256.0);
    return out;
}
Bytes encode_control(const std::vector<float>& values) {
    Bytes bytes;
    for (float v : values) {
        const auto bits = std::bit_cast<std::uint32_t>(v);
        for (unsigned i = 0; i < 4; ++i)
            bytes.push_back(static_cast<std::uint8_t>(bits >> (i * 8)));
    }
    return bytes;
}
std::vector<float> decode_control(const Bytes& bytes) {
    require(bytes.size() % 4 == 0, "saved control length");
    std::vector<float> out;
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        std::uint32_t bits = 0;
        for (unsigned k = 0; k < 4; ++k)
            bits |= std::uint32_t(bytes[i + k]) << (8 * k);
        const auto v = std::bit_cast<float>(bits);
        require(std::isfinite(v) && v >= 0 && v <= 1, "saved control sample");
        out.push_back(v);
    }
    return out;
}
void record(const fs::path& folder) {
    fs::create_directories(folder);
    const auto aa = encode(analyze(stimulus(0))), bb = encode(analyze(stimulus(1)));
    SignalGraph graph;
    const auto in = graph.add_input_node(1, "control"), out = graph.add_output_node(1, "audio");
    const auto source = graph.add_spectral_asset_morph_v1(aa.view(), bb.view(), 769, "assets");
    require(bool(source), "record source");
    const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
    const auto synth = graph.add_spectral_synthesis(shape, "synthesis");
    require(bool(synth), "record synthesis");
    require(bool(graph.connect(graph.node(in)->out("audio"), source.value->morph)),
            "record control");
    require(bool(graph.connect(source.value->frames, synth.value->frames)), "record spectral");
    require(bool(graph.connect(synth.value->audio[0], graph.node(out)->in("audio"))),
            "record audio");
    const auto saved = GraphSerializer::to_file(graph, folder / "graph.pulpgraph");
    require(saved.ok, "record graph file");
    write(folder / "asset-a.sfrm", Bytes(aa.view().begin(), aa.view().end()));
    write(folder / "asset-b.sfrm", Bytes(bb.view().begin(), bb.view().end()));
    write(folder / "control.f32", encode_control(controls()));
    std::string manifest = "pulp.a3-replay-inputs.v1\n";
    for (const char* name : {"graph.pulpgraph", "asset-a.sfrm", "asset-b.sfrm", "control.f32"})
        manifest += std::string(name) + " " + hash(read(folder / name)) + "\n";
    write(folder / "inputs.sha256", text_bytes(manifest));
}
int main(int argc, char** argv) {
    try {
        require(argc == 2 || (argc == 3 && std::string(argv[1]) == "--record"), "fixture argument");
        const fs::path folder(argv[argc - 1]);
        if (argc == 3)
            record(folder);
        const auto manifest = read(folder / "inputs.sha256");
        std::istringstream rows(std::string(manifest.begin(), manifest.end()));
        std::string schema, name, digest;
        std::getline(rows, schema);
        require(schema == "pulp.a3-replay-inputs.v1", "saved input schema");
        for (const char* expected_name :
             {"graph.pulpgraph", "asset-a.sfrm", "asset-b.sfrm", "control.f32"}) {
            require(bool(rows >> name >> digest) && name == expected_name, "saved input manifest");
            require(hash(read(folder / name)) == digest, "saved input digest");
        }
        require(!(rows >> name), "unsupported saved input");
        const auto a = analyze(stimulus(0)), b = analyze(stimulus(1));
        const auto aa = encode(a), bb = encode(b);
        require(read(folder / "asset-a.sfrm") == Bytes(aa.view().begin(), aa.view().end()),
                "asset A provenance");
        require(read(folder / "asset-b.sfrm") == Bytes(bb.view().begin(), bb.view().end()),
                "asset B provenance");
        const auto control = decode_control(read(folder / "control.f32"));
        require(control == controls(), "saved live timeline");
        const auto wanted = expected(a, b, control);
        const auto saved_bytes = read(folder / "graph.pulpgraph");
        const std::string saved(saved_bytes.begin(), saved_bytes.end());
        std::vector<float> reference;
        for (int partition : {32, 64, 128, 0}) {
            SignalGraph graph;
            const auto loaded = GraphSerializer::from_file(graph, folder / "graph.pulpgraph");
            require(loaded.ok, loaded.error.c_str());
            require(GraphSerializer::to_json(graph) + "\n" == saved, "save/load/save bytes");
            require(graph.prepare(48000, 4096), "replay prepare");
            require(graph.latency_samples() == 320, "replay latency");
            std::vector<float> audio(control.size());
            for (std::size_t t = 0; t < control.size();) {
                const auto count = int(std::min(
                    control.size() - t, std::size_t(partition ? partition : 1 + t * 17 % 511)));
                const float* in[] = {control.data() + t};
                float* out[] = {audio.data() + t};
                pulp::audio::BufferView<const float> iv(in, 1, count);
                pulp::audio::BufferView<float> ov(out, 1, count);
                graph.process(ov, iv, count);
                t += count;
            }
            compare(audio, wanted);
            require(reference.empty() || reference == audio, "replay partitions");
            reference = audio;
            for (std::size_t i = 0; i < audio.size(); ++i)
                if (i < 320 || i >= 1344)
                    require(audio[i] == 0, "replay startup/tail");
            require(graph.routed_walk_fallbacks() == 0, "replay executor");
            for (const auto& d : graph.poll_live_dsp_telemetry().spectral)
                require(d.first_error == 0, "replay diagnostics");
        }
        double peak = 0, square = 0;
        for (std::size_t i = 0; i < reference.size(); ++i) {
            const double error = std::abs(reference[i] - wanted[i]);
            peak = std::max(peak, error);
            square += error * error;
        }
        std::cout << std::setprecision(17) << "peak=" << peak
                  << " rms=" << std::sqrt(square / reference.size())
                  << " output_sha256=" << hash(encode_control(reference)) << '\n';
        std::cout << "A3 saved inputs, asset digests, DFT/Hann/OLA oracle and partitions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 42;
    }
}

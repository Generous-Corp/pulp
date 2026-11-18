// The single-stream spectral asset source through the public SignalGraph path:
// one decoded codec stream feeding synthesis at dividing and non-dividing hops.
// Assets come from an independent double-precision DFT on the analysis grid;
// the routed output is checked against an independent inverse DFT and
// squared-window OLA oracle, bit for bit against direct guarded synthesis of
// the stored frames, and against grid-walk counts for the explicit end.

#include "harness/rt_allocation_probe.hpp"
#include "spectral_asset_morph_internal.hpp"
#include "spectral_asset_source_internal.hpp"
#include "spectral_stream_oracle.hpp"

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/signal/spectral_frame_codec.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace oracle = pulp::test::spectral_oracle;
using oracle::Geometry;
using pulp::host::SpectralAssetSourceConfig;
using pulp::host::SpectralError;
using pulp::host::SpectralFiniteStreamRule;
using RuleStatus = SpectralFiniteStreamRule::Status;

namespace {

constexpr double sample_rate = 48000;
constexpr double peak_tolerance = 2e-5, rms_tolerance = 2e-6;
constexpr std::uint64_t bank_bytes = 64ull * 1024 * 1024;

struct SourceGraph {
    pulp::host::SignalGraph graph;
    pulp::host::SpectralAssetSourcePorts source;
    int channels = 0;

    SourceGraph(const Geometry& g, std::span<const std::uint8_t> bytes, std::uint64_t length)
        : channels(g.channels) {
        using namespace pulp::host;
        const auto output = graph.add_output_node(g.channels);
        const auto result = graph.add_spectral_asset_source_v1(bytes, length, "source");
        REQUIRE(result);
        source = *result.value;
        const auto identity = *SpectralIdentity::make(g.fft, g.hop, g.channels, sample_rate).value;
        REQUIRE(graph.node(source.node)->spectral_identity == identity);
        REQUIRE(graph.node(source.node)->input_ports.empty());
        const auto synthesis = *graph.add_spectral_synthesis(identity, "synthesis").value;
        REQUIRE(graph.connect(source.frames, synthesis.frames));
        for (int c = 0; c < g.channels; ++c)
            REQUIRE(graph.connect(synthesis.audio[c], graph.node(output)->input_ports[c]));
        REQUIRE(graph.prepare(sample_rate, 4096));
        REQUIRE(graph.latency_samples() == g.fft + g.hop);
    }

    // partition 0 is ragged: block sizes 1 to 511 that vary per block.
    oracle::Audio render(std::size_t total, int partition) {
        oracle::Audio audio(channels, std::vector<float>(total));
        std::vector<float*> outputs(channels);
        for (std::size_t pos = 0; pos < total;) {
            const auto count = std::min<std::size_t>(total - pos, partition ? std::size_t(partition)
                                                                            : 1 + pos * 17 % 511);
            for (int c = 0; c < channels; ++c)
                outputs[c] = audio[c].data() + pos;
            // The graph has no audio input; the view only carries the block length.
            pulp::audio::BufferView<const float> iv(nullptr, 0, count);
            pulp::audio::BufferView<float> ov(outputs.data(), channels, count);
            pulp::format::ProcessContext transport;
            std::size_t allocations = 0;
            {
                pulp::test::RtAllocationProbe probe;
                graph.process(ov, iv, int(count), transport);
                allocations = probe.allocation_count();
            }
            REQUIRE(allocations == 0);
            pos += count;
        }
        return audio;
    }

    void clean_diagnostics() {
        REQUIRE(graph.spectral_error() == SpectralError::None);
        for (const auto& diagnostic : graph.poll_live_dsp_telemetry().spectral)
            REQUIRE(diagnostic.first_error == 0);
        REQUIRE(graph.routed_walk_fallbacks() == 0);
    }
};

// Direct guarded synthesis of the stored frames plus the drain, read as far as
// the engine makes output final: what the routed path must reproduce exactly.
oracle::Audio direct_synthesis(const Geometry& g, std::span<const std::uint8_t> bytes,
                               std::uint64_t drain, std::size_t positions) {
    auto decoded = pulp::signal::decode_spectral_stream(bytes);
    REQUIRE(decoded);
    const auto& stream = *decoded.value;
    pulp::signal::SpectralFrameEngineConfig cfg;
    cfg.fft_size = g.fft;
    cfg.analysis_hop = g.hop;
    cfg.channels = g.channels;
    cfg.max_block = 4096;
    cfg.max_synthesis_hop = g.hop;
    cfg.defer_synthesis = false;
    cfg.guarded_split = true;
    pulp::signal::SpectralFrameEngine engine;
    engine.prepare(cfg);
    const int bins = g.fft / 2 + 1;
    std::vector<std::complex<float>> frame(std::size_t(g.channels) * bins);
    std::vector<const std::complex<float>*> pointers(g.channels);
    oracle::Audio out(g.channels, std::vector<float>(positions));
    std::size_t read = 0;
    for (std::uint64_t f = 0; f < stream.frame_count + drain; ++f) {
        for (std::size_t i = 0; i < frame.size(); ++i)
            frame[i] =
                f < stream.frame_count
                    ? std::complex<float>{stream.components[f * frame.size() * 2 + 2 * i],
                                          stream.components[f * frame.size() * 2 + 2 * i + 1]}
                    : std::complex<float>{};
        for (int c = 0; c < g.channels; ++c)
            pointers[c] = frame.data() + std::size_t(c) * bins;
        REQUIRE(engine.synthesize_frame_guarded(pointers.data()) ==
                pulp::signal::SpectralSplitResult::Ok);
        // Read every final sample; the next one must underrun.
        while (read < positions) {
            std::vector<float> one(g.channels);
            std::vector<float*> ptrs(g.channels);
            for (int c = 0; c < g.channels; ++c)
                ptrs[c] = one.data() + c;
            int consumed = 0;
            if (engine.read_output_guarded(ptrs.data(), 1, consumed) !=
                pulp::signal::SpectralSplitResult::Ok)
                break;
            for (int c = 0; c < g.channels; ++c)
                out[c][read] = one[c];
            ++read;
        }
    }
    REQUIRE(read == positions);
    return out;
}

void check_end(const Geometry& g, std::uint64_t length,
               const pulp::host::SpectralFiniteStreamEnd& end) {
    REQUIRE(end.policy_version == 2);
    REQUIRE(end.source_samples == length);
    REQUIRE(end.frame_count == oracle::grid_frames(g, length));
    REQUIRE(end.drain_frames == oracle::drain_frames(g));
    REQUIRE(static_cast<std::int64_t>(end.render_frames) ==
            oracle::last_window_end(g, length) + g.fft + g.hop);
}

void render_and_check(const Geometry& g, std::uint64_t length,
                      std::initializer_list<int> partitions) {
    CAPTURE(g.fft, g.hop, g.channels, length);
    const auto input = oracle::stimulus(g, length);
    const auto exact = oracle::analyze(g, input);
    REQUIRE(exact.size() == oracle::grid_frames(g, length));
    const auto bytes = oracle::encode(g, exact);
    const std::size_t delay = g.fft + g.hop;
    const auto end = std::size_t(oracle::last_window_end(g, length));
    const std::size_t cutoff = delay + end;
    const std::size_t total = cutoff + 3 * std::size_t(g.fft) + 101;

    // Calibrate the oracle without production DSP: from the exact frames it
    // reconstructs the input after the N + H delay.
    const auto calibrated = oracle::resynthesize(g, exact, total);
    for (int c = 0; c < g.channels; ++c)
        for (std::size_t t = 0; t < length; ++t)
            REQUIRE(std::abs(calibrated[c][t + delay] - input[c][t]) < 1e-11);
    const auto wanted = oracle::resynthesize(g, oracle::rounded(exact), total);
    const auto direct = direct_synthesis(g, bytes.view(), oracle::drain_frames(g), end);

    oracle::Audio reference;
    for (int partition : partitions) {
        CAPTURE(partition);
        SourceGraph fixture(g, bytes.view(), length);
        check_end(g, length, fixture.source.end);
        const auto audio = fixture.render(total, partition);
        oracle::compare(audio, wanted, peak_tolerance, rms_tolerance);
        REQUIRE((reference.empty() || audio == reference));
        reference = audio;
        for (int c = 0; c < g.channels; ++c) {
            for (std::size_t t = 0; t < total; ++t) {
                if (t < delay || t >= cutoff)
                    REQUIRE(audio[c][t] == 0);
                else
                    REQUIRE(audio[c][t] == direct[c][t - delay]);
            }
            // The final source sample, an impulse, is reconstructed.
            REQUIRE(std::abs(audio[c][delay + length - 1] - input[c][length - 1]) < 1e-4);
            REQUIRE(audio[c][delay + length - 1] != 0);
        }
        fixture.clean_diagnostics();
    }
}

} // namespace

TEST_CASE("Asset source reproduces the stored stream at dividing hops",
          "[host][spectral-asset-source][spectral-geometry]") {
    for (const Geometry g : {Geometry{256, 64, 1}, Geometry{512, 128, 2}, Geometry{1024, 256, 3}})
        render_and_check(g, 3 * std::uint64_t(g.fft) / 2 + 37, {32, 64, 4096, 0});
}

TEST_CASE("Asset source reproduces the stored stream at hops that do not divide the FFT",
          "[host][spectral-asset-source][spectral-geometry]") {
    for (const Geometry g :
         {Geometry{256, 63, 1}, Geometry{256, 100, 2}, Geometry{512, 96, 3}, Geometry{256, 3, 1}})
        render_and_check(g, 3 * std::uint64_t(g.fft) / 2 + 37, {1, 17, 63, 4096, 0});
    // FFT 1024 / hop 3: 341 leading frames.
    render_and_check({1024, 3, 2}, 211, {64, 0});
}

TEST_CASE("Asset source end is exact at short lengths and hop and window boundaries",
          "[host][spectral-asset-source][spectral-eof]") {
    for (const Geometry g : {Geometry{256, 63, 1}, Geometry{256, 100, 2}, Geometry{512, 96, 1},
                             Geometry{256, 64, 2}}) {
        const std::uint64_t h = g.hop, n = g.fft;
        for (const std::uint64_t length : {std::uint64_t{1}, std::uint64_t{2}, h - 1, h, h + 1,
                                           2 * h, 2 * h + 1, n - 1, n, n + 1, n + h})
            render_and_check(g, length, {17, 0});
    }
}

TEST_CASE("Asset source frames are the stored payload and keep the stored stamps",
          "[host][spectral-asset-source]") {
    const Geometry g{512, 96, 2};
    const std::uint64_t length = 700;
    const auto bytes = oracle::encode(g, oracle::analyze(g, oracle::stimulus(g, length)));
    const auto made = SpectralAssetSourceConfig::make(bytes.view(), length, bank_bytes);
    REQUIRE(made);
    const auto& config = **made.value;
    REQUIRE(config.rule.policy == SpectralFiniteStreamRule::Policy::analysis_grid_v2);
    REQUIRE(config.retained_bytes() ==
            sizeof(SpectralAssetSourceConfig) +
                config.stream.frame_count * sizeof(pulp::signal::SpectralFrameStamp) +
                config.stream.component_count * sizeof(float));
    std::vector<std::complex<float>> frame(std::size_t(g.channels) * (g.fft / 2 + 1));
    const auto first = oracle::grid_start(g);
    for (std::uint64_t f = 0; f < config.stream.frame_count; ++f) {
        const auto origin = first + std::int64_t(f) * g.hop;
        REQUIRE(config.frame(f, origin, frame));
        for (std::size_t i = 0; i < frame.size(); ++i) {
            REQUIRE(frame[i].real() == config.stream.components[f * frame.size() * 2 + 2 * i]);
            REQUIRE(frame[i].imag() == config.stream.components[f * frame.size() * 2 + 2 * i + 1]);
        }
        // A runtime origin off the stored stamp is refused, not read.
        REQUIRE_FALSE(config.frame(f, origin + 1, frame));
    }
    // Drain frames are silent.
    REQUIRE(config.frame(config.stream.frame_count, 0, frame));
    REQUIRE(std::all_of(frame.begin(), frame.end(),
                        [](auto value) { return value == std::complex<float>{}; }));
    std::vector<std::complex<float>> short_frame(frame.size() - 1);
    REQUIRE_FALSE(config.frame(0, first, short_frame));
}

TEST_CASE("Asset source prepares and stays silent at the largest admitted geometries",
          "[host][spectral-asset-source][spectral-capacity]") {
    for (const Geometry g : {Geometry{16384, 8192, 16}, Geometry{16384, 8191, 16}}) {
        CAPTURE(g.hop);
        const std::uint64_t length = 1;
        const auto bytes = oracle::zero_stream(g, oracle::grid_frames(g, length));
        SourceGraph fixture(g, bytes.view(), length);
        check_end(g, length, fixture.source.end);
        const auto audio = fixture.render(fixture.source.end.render_frames + 4096, 4096);
        for (const auto& channel : audio)
            REQUIRE(std::all_of(channel.begin(), channel.end(), [](float x) { return x == 0; }));
        fixture.clean_diagnostics();
    }
}

TEST_CASE("Asset source holds one stream where the morph needs two",
          "[host][spectral-asset-source][spectral-capacity]") {
    // About 128 KiB per frame at this geometry. One stream of 480 frames is
    // admitted and prepares with its synthesis inside the 64 MiB bank, where
    // the morph's two streams of 480 are refused. The codec's largest stream,
    // 511 frames, is admitted but cannot prepare: the bank refuses it rather
    // than retaining more than its bound.
    const Geometry g{4096, 2048, 8};
    const auto length_for = [&](std::uint64_t frames) { return (frames - 1) * 2048; };
    for (const std::uint64_t frames : {480ull, 511ull}) {
        CAPTURE(frames);
        REQUIRE(oracle::grid_frames(g, length_for(frames)) == frames);
        const auto bytes = oracle::zero_stream(g, frames);
        using namespace pulp::host;
        SignalGraph graph;
        const auto output = graph.add_output_node(g.channels);
        const auto source =
            graph.add_spectral_asset_source_v1(bytes.view(), length_for(frames), "one");
        REQUIRE(source);
        const auto identity = *SpectralIdentity::make(g.fft, g.hop, g.channels, sample_rate).value;
        const auto synthesis = *graph.add_spectral_synthesis(identity, "synthesis").value;
        REQUIRE(graph.connect(source.value->frames, synthesis.frames));
        for (int c = 0; c < g.channels; ++c)
            REQUIRE(graph.connect(synthesis.audio[c], graph.node(output)->input_ports[c]));
        REQUIRE(graph.prepare(sample_rate, 512) == (frames == 480));
        SignalGraph morph_graph;
        const auto morph = morph_graph.add_spectral_asset_morph_v1(bytes.view(), bytes.view(),
                                                                   length_for(frames), "two");
        REQUIRE_FALSE(morph);
        REQUIRE(morph.error == SpectralError::CapacityExceeded);
    }
}

TEST_CASE("Asset source refuses mismatched lengths, empty sources and inputs",
          "[host][spectral-asset-source][spectral-refusal]") {
    const Geometry g{256, 100, 2};
    const std::uint64_t length = 300;
    const auto bytes = oracle::zero_stream(g, oracle::grid_frames(g, length));
    const auto refused = [&](std::uint64_t l) {
        pulp::host::SignalGraph graph;
        const auto result = graph.add_spectral_asset_source_v1(bytes.view(), l, "refused");
        REQUIRE_FALSE(result);
        REQUIRE(graph.nodes().empty());
        return result.error;
    };
    // A length whose grid count differs from the stored count, and no length.
    REQUIRE(oracle::grid_frames(g, length + 100) != oracle::grid_frames(g, length));
    REQUIRE(refused(length + 100) == SpectralError::IdentityMismatch);
    REQUIRE(refused(0) == SpectralError::InvalidFrame);
    const std::uint8_t garbage[] = {1, 2, 3};
    {
        pulp::host::SignalGraph graph;
        REQUIRE(graph.add_spectral_asset_source_v1(garbage, length, "garbage").error ==
                SpectralError::InvalidFrame);
    }
    RuleStatus refusal = RuleStatus::ok;
    REQUIRE_FALSE(SpectralAssetSourceConfig::make(bytes.view(), 0, bank_bytes, &refusal));
    REQUIRE(refusal == RuleStatus::invalid_length);

    // Control: every length with the same grid count is admitted.
    for (std::uint64_t l = 1; l <= length + 100; ++l)
        if (oracle::grid_frames(g, l) == oracle::grid_frames(g, length)) {
            pulp::host::SignalGraph graph;
            REQUIRE(graph.add_spectral_asset_source_v1(bytes.view(), l, "kept"));
        }

    // The source takes no input: no numeric or spectral edge reaches it.
    using namespace pulp::host;
    SignalGraph graph;
    const auto input = graph.add_input_node(2);
    const auto source = *graph.add_spectral_asset_source_v1(bytes.view(), length, "kept").value;
    const auto identity = *SpectralIdentity::make(g.fft, g.hop, g.channels, sample_rate).value;
    const auto analysis = *graph.add_spectral_analysis(identity, "analysis").value;
    REQUIRE_FALSE(graph.connect(input, 0, source.node, 0));
    REQUIRE_FALSE(graph.connect(analysis.frames, SpectralInputPort{source.frames.handle}));
    // A source with no consumer cannot be prepared, and the writer refuses
    // to save the source at all.
    REQUIRE_FALSE(graph.prepare(sample_rate, 512));
    REQUIRE(GraphSerializer::to_json(graph).empty());
}

TEST_CASE("Asset source and morph nodes carry a minted authored ID like every other node",
          "[host][spectral-asset-source][authored-id]") {
    // A dividing hop, so the morph control below is admitted too.
    const Geometry g{256, 128, 2};
    const std::uint64_t length = 300;
    const auto bytes = oracle::zero_stream(g, oracle::grid_frames(g, length));
    using namespace pulp::host;
    SignalGraph graph;
    const auto output = graph.add_output_node(g.channels);
    const auto source = graph.add_spectral_asset_source_v1(bytes.view(), length, "a");
    const auto second = graph.add_spectral_asset_source_v1(bytes.view(), length, "b");
    // Control: a morph node goes through the shared minting path.
    const auto morph = graph.add_spectral_asset_morph_v1(bytes.view(), bytes.view(), length, "m");
    REQUIRE(source);
    REQUIRE(second);
    REQUIRE(morph);
    std::set<std::string> ids;
    for (const auto node : {output, source.value->node, second.value->node, morph.value->node}) {
        CAPTURE(node);
        const auto& id = graph.node(node)->authored_id;
        REQUIRE_FALSE(id.empty());
        REQUIRE(ids.insert(id.str()).second);
    }
}

TEST_CASE("Asset morph keeps the dividing-hop policy its control clock needs",
          "[host][spectral-asset-source][spectral-refusal]") {
    // The same non-dividing stream the source admits is refused by the morph,
    // whose frame f averages control hop f and so needs H to divide N.
    const Geometry g{256, 63, 1};
    const auto bytes = oracle::zero_stream(g, oracle::grid_frames(g, 1));
    RuleStatus refusal = RuleStatus::ok;
    REQUIRE_FALSE(pulp::host::SpectralAssetMorphConfig::make(bytes.view(), bytes.view(), 1,
                                                             bank_bytes, &refusal));
    REQUIRE(refusal == RuleStatus::hop_does_not_divide_fft_size);
    REQUIRE(SpectralAssetSourceConfig::make(bytes.view(), 1, bank_bytes, &refusal));
    REQUIRE(refusal == RuleStatus::unset);
}

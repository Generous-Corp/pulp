// The two-asset spectral source at codec geometries other than FFT 256 / hop 64
// mono. Each case analyzes audio on the analysis frame grid the engine starts
// on, with an independent double-precision DFT, and checks the routed output
// against an independent double-precision inverse DFT and squared-window OLA.

#include "harness/rt_allocation_probe.hpp"
#include "spectral_asset_morph_internal.hpp"

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
#include <limits>
#include <numbers>
#include <span>
#include <vector>

using pulp::host::SpectralAssetMorphConfig;
using pulp::host::SpectralError;
using pulp::host::SpectralFiniteStreamRule;
using RuleStatus = SpectralFiniteStreamRule::Status;

namespace {

struct Geometry {
    int fft, hop, channels;
};

// Non-default geometries: a finer hop at the default size, larger sizes, and
// two to three channels.
const Geometry rendered[] = {{256, 32, 1}, {512, 128, 1}, {1024, 256, 2}, {2048, 1024, 3}};

constexpr double peak_tolerance = 2e-5, rms_tolerance = 2e-6;
constexpr double sample_rate = 48000;
// The runtime bank's shared bound on retained assets, scratch and edges.
constexpr std::uint64_t bank_bytes = 64ull * 1024 * 1024;

using Frame = std::vector<std::complex<double>>; // channel-major, channels * bins
using Stream = std::vector<Frame>;

double window(int i, int n) {
    return 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / (n - 1));
}

std::int64_t grid_start(const Geometry& g) {
    return pulp::signal::spectral_frame_engine_first_frame_start(g.fft, g.hop, true);
}

// Analysis frames whose window [s, s + N) overlaps [0, L), walking the grid.
std::size_t grid_frames(const Geometry& g, std::size_t length) {
    std::size_t count = 0;
    for (auto s = grid_start(g); s < static_cast<std::int64_t>(length); s += g.hop)
        if (s + g.fft > 0)
            ++count;
    return count;
}

std::vector<std::vector<float>> stimulus(const Geometry& g, int endpoint, std::size_t length) {
    std::vector<std::vector<float>> audio(g.channels, std::vector<float>(length));
    for (int c = 0; c < g.channels; ++c) {
        const double cycle = 5 + 3 * c + 2 * endpoint;
        for (std::size_t i = 0; i < length; ++i)
            audio[c][i] = float((endpoint ? -0.06 : 0.04) + 0.02 * c +
                                0.12 * std::cos(2 * std::numbers::pi * cycle * double(i) / g.fft +
                                                (endpoint ? -3.1 : 3.1) + c) +
                                (i % 2 ? -0.03 : 0.03));
        audio[c][0] += 0.3f;
        audio[c][13 + c] -= 0.25f;
        audio[c].back() += endpoint ? -0.4f : 0.5f;
    }
    return audio;
}

Stream analyze(const Geometry& g, const std::vector<std::vector<float>>& audio) {
    const int bins = g.fft / 2 + 1;
    const auto length = audio.front().size();
    Stream frames(grid_frames(g, length), Frame(static_cast<std::size_t>(g.channels) * bins));
    for (std::size_t f = 0; f < frames.size(); ++f)
        for (int c = 0; c < g.channels; ++c)
            for (int k = 0; k < bins; ++k) {
                std::complex<double> sum{};
                for (int i = 0; i < g.fft; ++i) {
                    const auto t = grid_start(g) + static_cast<std::int64_t>(f) * g.hop + i;
                    if (t >= 0 && t < static_cast<std::int64_t>(length))
                        sum += double(audio[c][t]) * window(i, g.fft) *
                               std::polar(1.0, -2 * std::numbers::pi * k * i / g.fft);
                }
                frames[f][c * bins + k] =
                    k == 0 || k == bins - 1 ? std::complex<double>{sum.real(), 0} : sum;
            }
    return frames;
}

pulp::signal::EncodedSpectralStream encode(const Geometry& g, const Stream& frames,
                                           double rate = sample_rate) {
    const int bins = g.fft / 2 + 1;
    auto stream = pulp::signal::make_spectral_stream(
        {std::uint32_t(g.fft), std::uint32_t(g.hop), std::uint16_t(g.channels), rate},
        frames.size());
    REQUIRE(stream);
    // Codec layout: frame-major, then channel, then (re, im) per bin.
    const std::size_t stride = 2ull * g.channels * bins;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        REQUIRE(stream.value->frames[f].origin == grid_start(g) + std::int64_t(f) * g.hop);
        for (int c = 0; c < g.channels; ++c)
            for (int k = 0; k < bins; ++k) {
                const auto p = f * stride + 2ull * (c * bins + k);
                stream.value->components[p] = float(frames[f][c * bins + k].real());
                stream.value->components[p + 1] = float(frames[f][c * bins + k].imag());
            }
    }
    auto bytes = pulp::signal::encode_spectral_stream(*stream.value);
    REQUIRE(bytes);
    return std::move(*bytes.value);
}

std::complex<double> interpolate(std::complex<double> a, std::complex<double> b, double amount,
                                 bool self_conjugate) {
    if (amount == 0)
        return a;
    if (amount == 1)
        return b;
    const double ma = std::abs(a), mb = std::abs(b), magnitude = (1 - amount) * ma + amount * mb;
    double pa = std::arg(a), pb = std::arg(b);
    if (ma == 0)
        pa = pb;
    if (mb == 0)
        pb = pa;
    if (self_conjugate)
        return {std::cos(amount < 0.5 ? pa : pb) < 0 ? -magnitude : magnitude, 0};
    const double phase = pa + amount * std::remainder(pb - pa, 2 * std::numbers::pi);
    return {magnitude * std::cos(phase), magnitude * std::sin(phase)};
}

// Each frame f takes the mean of control hop f; N / H zero frames follow the
// stored frames and count toward the squared-window normalization.
std::vector<std::vector<double>> expected(const Geometry& g, const Stream& a, const Stream& b,
                                          const std::vector<float>& control) {
    const int bins = g.fft / 2 + 1;
    const std::size_t end = a.size() * g.hop, delay = g.fft + g.hop;
    std::vector<std::vector<double>> output(g.channels, std::vector<double>(control.size()));
    for (int c = 0; c < g.channels; ++c) {
        std::vector<double> numerator(end), norm(end);
        for (std::size_t f = 0; f < a.size() + g.fft / g.hop; ++f) {
            double amount = 0;
            for (int i = 0; i < g.hop; ++i)
                amount += control[f * g.hop + i] / double(g.hop);
            std::vector<std::complex<double>> frame(bins);
            if (f < a.size())
                for (int k = 0; k < bins; ++k)
                    frame[k] = interpolate(a[f][c * bins + k], b[f][c * bins + k], amount,
                                           k == 0 || k == bins - 1);
            for (int i = 0; i < g.fft; ++i) {
                const auto t = grid_start(g) + static_cast<std::int64_t>(f) * g.hop + i;
                if (t < 0 || t >= static_cast<std::int64_t>(end))
                    continue;
                double value = frame[0].real() + (i % 2 ? -1 : 1) * frame[bins - 1].real();
                for (int k = 1; k < bins - 1; ++k)
                    value +=
                        2 *
                        (frame[k] * std::polar(1.0, 2 * std::numbers::pi * k * i / g.fft)).real();
                numerator[t] += value / g.fft * window(i, g.fft);
                norm[t] += window(i, g.fft) * window(i, g.fft);
            }
        }
        for (std::size_t t = 0; t < end && t + delay < control.size(); ++t)
            output[c][t + delay] = norm[t] > 1e-9 ? numerator[t] / norm[t] : 0;
    }
    return output;
}

void compare(const std::vector<std::vector<float>>& actual,
             const std::vector<std::vector<double>>& wanted) {
    REQUIRE(actual.size() == wanted.size());
    for (std::size_t c = 0; c < actual.size(); ++c) {
        CAPTURE(c);
        REQUIRE(actual[c].size() == wanted[c].size());
        double peak = 0, square = 0;
        for (std::size_t i = 0; i < actual[c].size(); ++i) {
            REQUIRE(std::isfinite(actual[c][i]));
            const double error = std::abs(actual[c][i] - wanted[c][i]);
            peak = std::max(peak, error);
            square += error * error;
        }
        const double rms = std::sqrt(square / actual[c].size());
        CAPTURE(peak, rms);
        REQUIRE(peak <= peak_tolerance);
        REQUIRE(rms <= rms_tolerance);
    }
}

struct GeometryGraph {
    pulp::host::SignalGraph graph;
    pulp::host::SpectralAssetMorphPorts source;
    int channels = 0;

    GeometryGraph(const Geometry& g, std::span<const std::uint8_t> a,
                  std::span<const std::uint8_t> b, std::uint64_t length)
        : channels(g.channels) {
        using namespace pulp::host;
        const auto input = graph.add_input_node(1), output = graph.add_output_node(g.channels);
        const auto result = graph.add_spectral_asset_morph_v1(a, b, length, "assets");
        REQUIRE(result);
        source = *result.value;
        const auto identity = *SpectralIdentity::make(g.fft, g.hop, g.channels, sample_rate).value;
        const auto synthesis = *graph.add_spectral_synthesis(identity, "synthesis").value;
        REQUIRE(graph.connect(graph.node(input)->out("audio"), source.morph));
        REQUIRE(graph.connect(source.frames, synthesis.frames));
        REQUIRE(synthesis.audio.size() == std::size_t(g.channels));
        for (int c = 0; c < g.channels; ++c)
            REQUIRE(graph.connect(synthesis.audio[c], graph.node(output)->input_ports[c]));
        REQUIRE(graph.prepare(sample_rate, 4096));
        REQUIRE(graph.latency_samples() == g.fft + g.hop);
    }

    std::vector<std::vector<float>> render(const std::vector<float>& control, int partition) {
        std::vector<std::vector<float>> audio(channels, std::vector<float>(control.size()));
        std::vector<float*> outputs(channels);
        for (int pos = 0; pos < int(control.size());) {
            const auto count =
                std::min(int(control.size()) - pos, partition ? partition : 1 + pos * 17 % 511);
            const float* ip[] = {control.data() + pos};
            for (int c = 0; c < channels; ++c)
                outputs[c] = audio[c].data() + pos;
            pulp::audio::BufferView<const float> iv(ip, 1, count);
            pulp::audio::BufferView<float> ov(outputs.data(), channels, count);
            pulp::format::ProcessContext transport;
            std::size_t allocations = 0;
            {
                pulp::test::RtAllocationProbe probe;
                graph.process(ov, iv, count, transport);
                allocations = probe.allocation_count();
            }
            REQUIRE(allocations == 0);
            pos += count;
        }
        return audio;
    }

    void clean_diagnostics() {
        for (const auto& diagnostic : graph.poll_live_dsp_telemetry().spectral)
            REQUIRE(diagnostic.first_error == 0);
        REQUIRE(graph.routed_walk_fallbacks() == 0);
    }
};

SpectralError public_refusal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b,
                             std::uint64_t length) {
    pulp::host::SignalGraph graph;
    const auto result = graph.add_spectral_asset_morph_v1(a, b, length, "refused");
    REQUIRE_FALSE(result);
    REQUIRE(graph.nodes().empty());
    return result.error;
}

pulp::signal::EncodedSpectralStream zero_stream(const Geometry& g, std::uint64_t frames) {
    auto stream = pulp::signal::make_spectral_stream(
        {std::uint32_t(g.fft), std::uint32_t(g.hop), std::uint16_t(g.channels), sample_rate},
        frames);
    REQUIRE(stream);
    auto bytes = pulp::signal::encode_spectral_stream(*stream.value);
    REQUIRE(bytes);
    return std::move(*bytes.value);
}

} // namespace

TEST_CASE("Asset morph renders the analysis grid at non-default codec geometries",
          "[host][spectral-asset-morph][spectral-geometry]") {
    for (const auto& g : rendered) {
        CAPTURE(g.fft, g.hop, g.channels);
        const std::size_t length = 3 * g.fft / 2 + 37;
        const auto input_a = stimulus(g, 0, length), input_b = stimulus(g, 1, length);
        const auto a = analyze(g, input_a), b = analyze(g, input_b);
        const auto bytes_a = encode(g, a), bytes_b = encode(g, b);
        const auto cutoff = std::size_t(g.fft + g.hop) + a.size() * g.hop;
        const std::size_t delay = g.fft + g.hop;
        for (float amount : {0.0f, 1.0f, 0.37f, -1.0f}) {
            CAPTURE(amount);
            std::vector<float> control(cutoff + 2 * g.fft + 64, amount);
            if (amount < 0)
                for (std::size_t i = 0; i < control.size(); ++i)
                    control[i] = float((i * 13 % 257) / 256.0);
            const auto wanted = expected(g, a, b, control);
            // Calibrate the oracle: at an endpoint it reconstructs the input
            // after the N + H delay without calling production DSP.
            if (amount == 0 || amount == 1)
                for (int c = 0; c < g.channels; ++c)
                    for (std::size_t t = 0; t < length; ++t)
                        REQUIRE(std::abs(wanted[c][t + delay] -
                                         (amount == 0 ? input_a : input_b)[c][t]) < 1e-11);
            std::vector<std::vector<float>> reference;
            for (int partition : {32, 64, 128, 0, 4096}) {
                CAPTURE(partition);
                GeometryGraph fixture(g, bytes_a.view(), bytes_b.view(), length);
                REQUIRE(fixture.source.render_frames == cutoff);
                const auto audio = fixture.render(control, partition);
                compare(audio, wanted);
                REQUIRE((reference.empty() || audio == reference));
                reference = audio;
                for (int c = 0; c < g.channels; ++c)
                    for (std::size_t t = 0; t < audio[c].size(); ++t)
                        if (t < delay || t >= cutoff)
                            REQUIRE(audio[c][t] == 0);
                fixture.clean_diagnostics();
            }
        }
    }
}

TEST_CASE("Asset morph prepares and stays silent at the largest admitted geometry",
          "[host][spectral-asset-morph][spectral-geometry]") {
    const Geometry g{16384, 8192, 16};
    // One source sample spans two frames on this grid.
    const auto bytes = zero_stream(g, 2);
    GeometryGraph fixture(g, bytes.view(), bytes.view(), 1);
    REQUIRE(fixture.source.render_frames == 16384 + 8192 + 2 * 8192);
    const auto audio =
        fixture.render(std::vector<float>(fixture.source.render_frames + 4096, 0.5f), 4096);
    for (const auto& channel : audio)
        REQUIRE(std::all_of(channel.begin(), channel.end(), [](float x) { return x == 0; }));
    fixture.clean_diagnostics();
}

TEST_CASE("Asset morph refuses a hop that does not divide the FFT size by name",
          "[host][spectral-asset-morph][spectral-refusal]") {
    // The codec decodes these geometries; only the finite-stream rule refuses.
    for (const Geometry g : {Geometry{256, 100, 1}, Geometry{512, 96, 2}, Geometry{1024, 3, 1}}) {
        CAPTURE(g.fft, g.hop, g.channels);
        const std::uint64_t length = 1;
        // A frame count matching the analysis grid, so no count check refuses.
        const auto bytes = zero_stream(g, grid_frames(g, length));
        RuleStatus refusal = RuleStatus::ok;
        const auto config = SpectralAssetMorphConfig::make(bytes.view(), bytes.view(), length,
                                                           bank_bytes, &refusal);
        REQUIRE_FALSE(config);
        REQUIRE(config.error == SpectralError::UnsupportedOperation);
        REQUIRE(refusal == RuleStatus::hop_does_not_divide_fft_size);
        REQUIRE(std::strcmp(SpectralFiniteStreamRule::reason(refusal),
                            "hop does not divide the fft size") == 0);
        REQUIRE(public_refusal(bytes.view(), bytes.view(), length) ==
                SpectralError::UnsupportedOperation);
    }
    // Control: a dividing hop at the same size and length is admitted.
    const Geometry admitted{256, 128, 1};
    const auto bytes = zero_stream(admitted, grid_frames(admitted, 1));
    RuleStatus refusal = RuleStatus::ok;
    REQUIRE(SpectralAssetMorphConfig::make(bytes.view(), bytes.view(), 1, bank_bytes, &refusal));
    REQUIRE(refusal == RuleStatus::unset);
}

TEST_CASE("Asset morph refuses geometry the codec cannot carry and mismatched endpoints",
          "[host][spectral-asset-morph][spectral-refusal]") {
    // Outside the codec range nothing can be encoded, so no asset reaches the
    // finite-stream rule.
    for (const auto bad : {pulp::signal::SpectralStreamGeometry{128, 64, 1, sample_rate},
                           pulp::signal::SpectralStreamGeometry{32768, 64, 1, sample_rate},
                           pulp::signal::SpectralStreamGeometry{256, 129, 1, sample_rate},
                           pulp::signal::SpectralStreamGeometry{256, 64, 17, sample_rate}})
        REQUIRE_FALSE(pulp::signal::make_spectral_stream(bad, 4));

    const Geometry stereo{1024, 256, 2}, mono{1024, 256, 1}, other_hop{1024, 128, 2};
    const std::uint64_t length = 700;
    const auto a = zero_stream(stereo, grid_frames(stereo, length));
    REQUIRE(public_refusal(a.view(), zero_stream(mono, grid_frames(mono, length)).view(), length) ==
            SpectralError::IdentityMismatch);
    REQUIRE(public_refusal(a.view(), zero_stream(other_hop, grid_frames(other_hop, length)).view(),
                           length) == SpectralError::IdentityMismatch);
    // A frame count that is not the grid count for the stated length.
    REQUIRE(public_refusal(a.view(), a.view(), length + 256) == SpectralError::IdentityMismatch);
    REQUIRE(public_refusal(a.view(), a.view(), 0) == SpectralError::InvalidFrame);

    // Control: the same stream with its own grid length is admitted. A graph
    // holding only the source (no synthesis or output) cannot be prepared, so
    // the writer, which saves only graphs its reader can restore, refuses it.
    pulp::host::SignalGraph graph;
    const auto accepted = graph.add_spectral_asset_morph_v1(a.view(), a.view(), length, "kept");
    REQUIRE(accepted);
    REQUIRE(accepted.value->render_frames == 1024 + 256 + grid_frames(stereo, length) * 256);
    REQUIRE(pulp::host::GraphSerializer::to_json(graph).empty());
}

TEST_CASE("Asset morph bounds retained assets at large geometry by the runtime bank",
          "[host][spectral-asset-morph][spectral-capacity]") {
    // About 128 KiB per frame per stream at this geometry; two streams of 240
    // frames fit the 64 MiB bank and two of 260 do not.
    const Geometry g{4096, 2048, 8};
    constexpr std::uint64_t frame_bytes = 2ull * 8 * 2049 * sizeof(float);
    const auto length_for = [&](std::uint64_t frames) { return (frames - 1) * 2048; };
    for (std::uint64_t frames : {240ull, 260ull}) {
        CAPTURE(frames);
        REQUIRE(grid_frames(g, length_for(frames)) == frames);
        const auto bytes = zero_stream(g, frames);
        const bool fits = 2 * frames * frame_bytes < bank_bytes;
        REQUIRE(fits == (frames == 240));
        pulp::host::SignalGraph graph;
        const auto result = graph.add_spectral_asset_morph_v1(bytes.view(), bytes.view(),
                                                              length_for(frames), "big");
        REQUIRE(bool(result) == fits);
        if (!fits)
            REQUIRE(result.error == SpectralError::CapacityExceeded);
    }
}

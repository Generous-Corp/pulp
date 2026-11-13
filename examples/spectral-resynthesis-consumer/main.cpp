// Installed-SDK spectral resynthesis consumer: analysis -> stage -> synthesis
// through the public routed spectral graph seams. It records the frame geometry,
// measures latency, the finite tail, partition invariance and reconstruction
// error, and runs negative controls that each check must reject.
#include <pulp/graph/spectral_frame.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using namespace pulp::host;
using Channels = std::vector<std::vector<float>>;

constexpr int kFft = 512, kHop = 128, kChannels = 2, kTotal = 8192, kLength = 6000;
constexpr double kRate = 48000.0;
// Identity reconstruction tolerance: residual energy / signal energy per channel.
constexpr double kIdentityTolerance = 1e-10; // -100 dB
constexpr int kToneBinA = 40, kToneBinB = 120, kBandHalfWidth = 8;
constexpr float kBandGain = 0.25f;

void require(bool ok, const std::string& what) {
    if (!ok)
        throw std::runtime_error("FAIL: " + what);
}

enum class Stage { Identity, Uniform, Band };
struct Spec {
    Stage stage = Stage::Identity;
    float uniform_gain = 1.0f;
    int analysis_hop = kHop; // planted mutation points
    int synthesis_hop = kHop;
};

struct Render {
    Channels out;
    int reported_latency = 0;
    std::int64_t snapshot_latency = 0;
    std::uint64_t fallbacks = 0;
};

Render render(const Spec& spec, const Channels& in, int partition, bool parallel = false) {
    const auto shape = *SpectralIdentity::make(kFft, spec.analysis_hop, kChannels, kRate).value;
    auto synth_shape = SpectralIdentity::make(kFft, spec.synthesis_hop, kChannels, kRate);
    require(bool(synth_shape), "synthesis identity");
    std::vector<float> gains(static_cast<size_t>(kChannels) * shape.bins(), spec.uniform_gain);
    if (spec.stage == Stage::Band)
        for (int c = 0; c < kChannels; ++c)
            for (int k = kToneBinA - kBandHalfWidth; k <= kToneBinA + kBandHalfWidth; ++k)
                gains[static_cast<size_t>(c) * shape.bins() + k] = kBandGain;
    auto stage = spec.stage == Stage::Identity ? QualifiedSpectralStage::identity_v1(shape)
                                               : QualifiedSpectralStage::real_mask_v1(shape, gains);
    require(bool(stage), "qualified stage");

    SignalGraph graph;
    const auto input = graph.add_input_node(kChannels);
    const auto output = graph.add_output_node(kChannels);
    const auto analysis = *graph.add_spectral_analysis(shape, "analysis").value;
    const auto transform = *graph.add_spectral_stage(shape, *stage.value, "transform").value;
    auto synthesis = graph.add_spectral_synthesis(*synth_shape.value, "resynthesis");
    require(bool(synthesis), "synthesis node");
    for (int c = 0; c < kChannels; ++c)
        require(bool(graph.connect(input, c, analysis.node, c)), "audio -> analysis");
    require(bool(graph.connect(analysis.frames, transform.frames_in)), "analysis -> stage");
    require(bool(graph.connect(transform.frames_out, synthesis.value->frames)),
            "stage -> synthesis geometry match");
    for (int c = 0; c < kChannels; ++c)
        require(bool(graph.connect(synthesis.value->node, c, output, c)), "synthesis -> out");

    // Geometry recorded on the authored nodes must be the declared geometry.
    require(graph.node(analysis.node)->spectral_identity == shape, "analysis geometry");
    require(graph.node(synthesis.value->node)->spectral_identity == shape, "synthesis geometry");
    require(graph.node(analysis.node)->spectral_identity->frame_contract().encode() ==
                shape.frame_contract().encode(),
            "analysis wire contract");

    graph.set_parallel_routing_enabled(parallel);
    require(graph.prepare(kRate, 4096), "prepare");
    Render r;
    r.reported_latency = graph.latency_samples();
    r.snapshot_latency = graph.prepared_latency_snapshot()->total_latency_samples;
    r.out.assign(kChannels, std::vector<float>(kTotal));
    for (int pos = 0; pos < kTotal;) {
        const int count = std::min(kTotal - pos, partition ? partition : 1 + (pos * 17 + 5) % 4096);
        const float* ip[kChannels];
        float* op[kChannels];
        for (int c = 0; c < kChannels; ++c) {
            ip[c] = in[c].data() + pos;
            op[c] = r.out[c].data() + pos;
        }
        pulp::audio::BufferView<const float> iv(ip, kChannels, count);
        pulp::audio::BufferView<float> ov(op, kChannels, count);
        graph.process(ov, iv, count);
        pos += count;
    }
    require(graph.spectral_error() == SpectralError::None, "no sticky spectral error");
    r.fallbacks = graph.routed_walk_fallbacks();
    return r;
}

double tone_amplitude(const std::vector<float>& y, int start, int length, int bin) {
    // Bin-centred tones complete whole cycles over a multiple of N samples, so
    // a single-frequency projection is exact (no leakage between the tones).
    double a = 0, b = 0;
    const double w = 2.0 * M_PI * bin / kFft;
    for (int i = start; i < start + length; ++i) {
        a += y[i] * std::cos(w * i);
        b += y[i] * std::sin(w * i);
    }
    return 2.0 * std::sqrt(a * a + b * b) / length;
}

double residual_ratio(const Channels& in, const Channels& out, int delay, double gain) {
    double worst = 0;
    for (int c = 0; c < kChannels; ++c) {
        double energy = 0, error = 0;
        for (int i = 0; i < kTotal; ++i) {
            const double wanted = i >= delay ? gain * in[c][i - delay] : 0.0;
            energy += wanted * wanted;
            error += (out[c][i] - wanted) * (out[c][i] - wanted);
        }
        worst = std::max(worst, error / energy);
    }
    return worst;
}

int argmax_abs(const std::vector<float>& y) {
    return int(std::max_element(y.begin(), y.end(),
                                [](float a, float b) { return std::abs(a) < std::abs(b); }) -
               y.begin());
}
} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc > 1 ? argv[1] : "";
    try {
        // (1) Explicit frame geometry: declared, encoded, decoded, checked.
        const auto shape = *SpectralIdentity::make(kFft, kHop, kChannels, kRate).value;
        auto wire = shape.frame_contract().encode();
        if (mode == "--planted-window")
            wire[24] = 2; // a window profile the contract does not define
        const auto decoded = pulp::graph::SpectralFrameContract::decode(wire);
        require(bool(decoded), "geometry contract decode");
        const auto& g = *decoded.value;
        require(g == shape.frame_contract() && g.fft_size == kFft && g.hop == kHop &&
                    g.channels == kChannels && g.bins == kFft / 2 + 1 && g.window == 1 &&
                    g.phase == 1 && g.normalization == 1 && g.precision == 1 &&
                    shape.sample_rate() == kRate,
                "recorded geometry");
        std::printf(
            "geometry: fft=%u hop=%u channels=%u bins=%u rate=%.0f window=hann.symmetric.v1 "
            "normalization=squared-window-wola.v1 layout=indexed\n",
            g.fft_size, g.hop, unsigned(g.channels), unsigned(g.bins), kRate);

        Spec identity;
        if (mode == "--planted-synthesis-hop")
            identity.synthesis_hop = kHop / 2; // synthesis geometry disagrees with analysis
        if (mode == "--planted-geometry-drift")
            identity.analysis_hop = identity.synthesis_hop = kHop / 2; // runtime != receipt

        // (2) Latency: impulse delay must equal the reported PDC (N + H).
        Channels impulses(kChannels, std::vector<float>(kTotal));
        impulses[0][0] = 1.0f;
        impulses[1][1000] = 1.0f;
        const auto imp = render(identity, impulses, 64);
        require(imp.reported_latency == kFft + kHop && imp.snapshot_latency == kFft + kHop,
                "reported latency N+H");
        const int d0 = argmax_abs(imp.out[0]), d1 = argmax_abs(imp.out[1]) - 1000;
        require(d0 == imp.reported_latency && d1 == imp.reported_latency,
                "measured impulse delay == reported latency (layout preserved)");
        std::printf("latency: reported=%d snapshot=%lld measured_ch0=%d measured_ch1=%d\n",
                    imp.reported_latency, (long long)imp.snapshot_latency, d0, d1);
        const int D = imp.reported_latency;
        // Instrument control: moving the impulse by one sample moves the reading.
        Channels shifted(kChannels, std::vector<float>(kTotal));
        shifted[0][1] = shifted[1][1001] = 1.0f;
        const auto imp1 = render(identity, shifted, 64);
        require(argmax_abs(imp1.out[0]) == D + 1 && argmax_abs(imp1.out[1]) == D + 1001,
                "latency instrument tracks a one-sample shift");

        // Finite source: two bin-centred tones plus edge impulses; EOF at kLength,
        // after which the host feeds zeros (the routed graph has no EOF signal).
        Channels src(kChannels, std::vector<float>(kTotal));
        for (int i = 0; i < kLength; ++i) {
            src[0][i] = 0.3f * std::sin(2.0 * M_PI * kToneBinA * i / kFft) +
                        0.2f * std::sin(2.0 * M_PI * kToneBinB * i / kFft);
            src[1][i] = 0.25f * std::cos(0.0371 * i) * std::sin(0.0011 * i);
        }
        src[0][kLength - 1] = src[1][kLength - 1] = 0.9f;

        // (5) Partition invariance, bitwise, across four partitions and both stages.
        const auto ref = render(identity, src, 64);
        for (int p : {4096, 37, 0})
            require(render(identity, src, p).out == ref.out, "identity partition invariance");
        Spec band = identity;
        band.stage = Stage::Band;
        const auto band_ref = render(band, src, 64);
        require(band_ref.out != ref.out, "invariance instrument sees a different stage");
        for (int p : {4096, 37, 0})
            require(render(band, src, p).out == band_ref.out, "band partition invariance");

        // Identity reconstruction with a stated tolerance.
        const double floor = residual_ratio(src, ref.out, D, 1.0);
        require(floor < kIdentityTolerance, "identity reconstruction within -100 dB");
        // Negative control: a -80 dB uniform defect must exceed the tolerance, and
        // the same instrument must accept it once the expected gain is applied.
        Spec uniform = identity;
        uniform.stage = Stage::Uniform;
        uniform.uniform_gain = 1.0f - 1e-4f;
        const auto defect = render(uniform, src, 64);
        const double seen = residual_ratio(src, defect.out, D, 1.0);
        require(seen > kIdentityTolerance, "negative control: defect visible");
        require(residual_ratio(src, defect.out, D, 1.0f - 1e-4f) < kIdentityTolerance,
                "negative control: defect explained by its gain");
        std::printf("reconstruction: identity_residual=%.3g tolerance=%.0e "
                    "planted_-80dB_defect_residual=%.3g\n",
                    floor, kIdentityTolerance, seen);

        // (3) EOF/tail: output support is [D, D + L + N - 1); zero outside it.
        int last = -1;
        for (int c = 0; c < kChannels; ++c)
            for (int i = 0; i < kTotal; ++i) {
                if (band_ref.out[c][i] != 0.0f || ref.out[c][i] != 0.0f)
                    last = std::max(last, i);
                if (i < D || i >= D + kLength + kFft - 1)
                    require(ref.out[c][i] == 0.0f && band_ref.out[c][i] == 0.0f,
                            "prefix/tail silence outside [D, D+L+N-1)");
            }
        // The masked stage spreads the final impulse past D+L-1, so the permitted
        // N-1 tail is exercised rather than vacuous.
        require(last > D + kLength - 1, "tail extends past the delayed EOF sample");
        std::printf("tail: L=%d support=[%d,%d) last_nonzero=%d\n", kLength, D,
                    D + kLength + kFft - 1, last);

        // Planted spectral transform: the masked band is attenuated, the other passes.
        const int start = D + 2 * kFft, span = 4 * kFft;
        const double a_id = tone_amplitude(ref.out[0], start, span, kToneBinA);
        const double a_band = tone_amplitude(band_ref.out[0], start, span, kToneBinA);
        const double b_band = tone_amplitude(band_ref.out[0], start, span, kToneBinB);
        require(std::abs(a_id - 0.3) < 1e-4, "identity tone A amplitude");
        require(std::abs(a_band / 0.3 - kBandGain) < 1e-3, "band gain on tone A");
        require(std::abs(b_band / 0.2 - 1.0) < 1e-3, "tone B outside band unchanged");
        std::printf("transform: toneA identity=%.6f band=%.6f (gain %.4f) toneB=%.6f\n", a_id,
                    a_band, a_band / 0.3, b_band);

        // (4) CPU path: the routed spectral runtime is CPU-only; the serial walk
        // must not fall back, and the parallel routing request must not change it.
        require(ref.fallbacks == 0 && band_ref.fallbacks == 0, "no routed walk fallback");
        require(render(identity, src, 64, true).out == ref.out, "parallel request == serial");
        std::printf("cpu: routed_walk_fallbacks=0 parallel_request_identical=1\n");

        std::printf("pulp.spectral-resynthesis-consumer.v1: all checks passed\n");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

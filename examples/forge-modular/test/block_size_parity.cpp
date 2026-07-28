// Cross-target parity: does shared DSP sound the same in a DAW plugin and in a
// Rack module?
//
// The two products drive the SAME graph at different block sizes -- a DAW host
// hands Pulp blocks of 64-4096 frames, while Rack calls process() once per
// sample. That difference is invisible until it isn't, so this gate renders one
// graph both ways and compares sample by sample.
//
// It encodes a real, measured asymmetry rather than asserting a blanket
// equality:
//
//   * A feedback-FREE graph is bit-identical at block 1 and block 64. Any drift
//     here is a genuine bug in the executor or the bake.
//   * A graph with a FEEDBACK edge is NOT identical, and legitimately so:
//     SignalGraph::connect_feedback is a ONE-BLOCK delay, so its delay TIME
//     scales with the block size -- 64 samples at block 64, 1 sample at block 1.
//     The module is arguably more correct (a 1-sample feedback is what a patch
//     cable does), but "same DSP, different sound across targets" must be a
//     declared property, not a discovery. This test pins the difference so it
//     cannot change silently.
//
// Build (needs a configured Pulp build for the static libs):
//   see examples/forge-modular/test/README.md

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using pulp::host::SignalGraph;
using pulp::host::bake;

constexpr double kSr = 48000.0;
constexpr int kFrames = 4096;

std::vector<float> tone(int n) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] =
            0.5f * std::sin(2.0f * 3.14159265358979f * 440.0f * static_cast<float>(i) /
                            static_cast<float>(kSr));
    return v;
}

void build(SignalGraph& g, bool feedback) {
    const auto in = g.add_input_node(1, "In");
    const auto a = g.add_gain_node("A");
    const auto b = g.add_gain_node("B");
    const auto out = g.add_output_node(1, "Out");
    g.connect(in, 0, a, 0);
    g.connect(in, 0, b, 0);
    if (feedback) g.connect_feedback(a, 0, a, 0);
    g.connect(a, 0, out, 0);
    g.connect(b, 0, out, 0);
    g.set_node_gain(a, 0.4101011f);
    g.set_node_gain(b, 0.1717171f);
}

/// Bake the graph and render it at a fixed block size.
bool render(bool feedback, int block, std::vector<float>& out) {
    SignalGraph g;
    build(g, feedback);
    if (!g.prepare(kSr, block)) {
        std::printf("  graph.prepare(sr, %d) REFUSED\n", block);
        return false;
    }
    auto r = bake(g);
    if (!r.accepted || !r.processor) {
        std::printf("  bake() REFUSED at block %d (reason %d)\n", block,
                    static_cast<int>(r.reason));
        return false;
    }
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = kSr;
    ctx.max_buffer_size = block;
    ctx.input_channels = 1;
    ctx.output_channels = 1;
    r.processor->prepare(ctx);

    const auto input = tone(kFrames);
    out.assign(static_cast<std::size_t>(kFrames), 0.0f);
    pulp::midi::MidiBuffer mi, mo;
    pulp::format::ProcessContext pc;
    pc.sample_rate = kSr;
    for (int off = 0; off < kFrames; off += block) {
        const float* ip = input.data() + off;
        float* op = out.data() + off;
        const float* ins[1] = {ip};
        float* outs[1] = {op};
        pulp::audio::BufferView<const float> iv(ins, 1, static_cast<std::uint32_t>(block));
        pulp::audio::BufferView<float> ov(outs, 1, static_cast<std::uint32_t>(block));
        mi.clear();
        mo.clear();
        r.processor->process(ov, iv, mi, mo, pc);
    }
    return true;
}

double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return m;
}

}  // namespace

int main() {
    int failures = 0;
    std::printf("cross-target parity: block 1 (Rack) vs block 64 (DAW)\n\n");

    // 1. Feedback-free: must be bit-identical. This is the real gate.
    {
        std::vector<float> a, b;
        if (!render(false, 64, a) || !render(false, 1, b)) return 2;
        const double d = max_abs_diff(a, b);
        const bool ok = d == 0.0;
        std::printf("  feedback-free graph      max|diff| = %.9g   %s\n", d,
                    ok ? "BIT-IDENTICAL" : "<-- FAIL, executor/bake bug");
        if (!ok) ++failures;
    }

    // 2. With feedback: expected to differ, because connect_feedback is a
    //    one-BLOCK delay. Pinned so the asymmetry cannot change unnoticed --
    //    and so that it showing up as zero would also be a signal.
    {
        std::vector<float> a, b;
        if (!render(true, 64, a) || !render(true, 1, b)) return 2;
        const double d = max_abs_diff(a, b);
        const bool differs = d > 1e-6;
        std::printf("  feedback graph           max|diff| = %.9g   %s\n", d,
                    differs ? "DIFFERS (expected: one-block delay scales with block size)"
                            : "<-- unexpected match; the feedback semantics changed");
        if (!differs) ++failures;
    }

    std::printf("\n%s\n", failures ? "PARITY GATE FAILED"
                                   : "parity gate OK (1 identity, 1 declared difference)");
    return failures ? 1 : 0;
}

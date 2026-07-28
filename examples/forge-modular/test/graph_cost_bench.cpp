// What does a BAKED GRAPH cost, per sample, versus hand-written per-sample DSP,
// in the driving pattern Rack actually uses?
//
// RESULT (Apple M3 Ultra, 48kHz, 2026-07-28) -- this decided the DSP-generation
// design, so re-run it before changing that decision:
//
//     hand-written SVF               7.9 ns   0.04% of a core
//     baked graph, 4 nodes, block 1  190.7 ns 0.92%   (24x)   <- not viable
//     same graph re-blocked at 8      24.5 ns 0.12%   (3.1x)  <- viable
//     same graph re-blocked at 16     12.8 ns 0.06%   (1.6x)
//
//     20-module rack: hand-written 0.8% | block-1 graphs 17.8% | block-8 2.4%
//
// The cost is dominated by a fixed ~90-120ns per process() call, not by the DSP
// inside it, so driving a baked graph one sample at a time pays that toll 48000
// times a second. Re-blocking 8-16 samples inside the module amortizes it away
// for 0.2-0.3ms of latency -- the same trade Surge XT Rack makes at 8 samples.
//
// This is the number the DSP-generation path lives or dies on. Generated
// modules would compose a baked SignalGraph (the way Forge's DAW products
// already do); hand-written modules call pulp::signal primitives directly. If
// the graph overhead is small, generation is free. If it is large, generated
// modules cost visibly more on Rack's per-module CPU meter.
#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/svf.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using pulp::host::SignalGraph;
using pulp::host::bake;
static constexpr double kSr = 48000.0;
static constexpr int kN = 48000 * 4;   // 4 seconds

static std::vector<float> tone() {
    std::vector<float> v(kN);
    for (int i = 0; i < kN; ++i)
        v[i] = 0.4f * std::sinf(2.0f * 3.14159265f * 220.0f * i / (float)kSr);
    return v;
}

// ── Hand-written: exactly what examples/forge-modular/src/voice.cpp VCF does ──
static double hand_written(const std::vector<float>& in, std::vector<float>& out) {
    pulp::signal::SvfT<float> f;
    f.set_sample_rate((float)kSr);
    f.set_mode(pulp::signal::SvfT<float>::Mode::lowpass);
    f.set_resonance(2.0f);
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) {
        f.set_frequency(1000.0f);       // params re-read per sample, as the module does
        out[i] = f.process(in[i]);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / kN;
}

// ── Baked graph driven one sample at a time, as Rack would ───────────────────
static double baked_graph(int nodes, const std::vector<float>& in,
                          std::vector<float>& out, bool& ok) {
    SignalGraph g;
    const auto ni = g.add_input_node(1, "In");
    const auto no = g.add_output_node(1, "Out");
    auto prev = ni;
    for (int i = 0; i < nodes; ++i) {
        const auto gn = g.add_gain_node("G");
        g.connect(prev, 0, gn, 0);
        g.set_node_gain(gn, 0.9f);
        prev = gn;
    }
    g.connect(prev, 0, no, 0);
    if (!g.prepare(kSr, 1)) { ok = false; return 0; }
    auto r = bake(g);
    if (!r.accepted || !r.processor) { ok = false; return 0; }
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = kSr; ctx.max_buffer_size = 1;
    ctx.input_channels = 1; ctx.output_channels = 1;
    r.processor->prepare(ctx);

    pulp::midi::MidiBuffer mi, mo;
    pulp::format::ProcessContext pc; pc.sample_rate = kSr;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) {
        const float* ip = in.data() + i;
        float* op = out.data() + i;
        const float* ins[1] = {ip}; float* outs[1] = {op};
        pulp::audio::BufferView<const float> iv(ins, 1, 1);
        pulp::audio::BufferView<float> ov(outs, 1, 1);
        mi.clear(); mo.clear();
        r.processor->process(ov, iv, mi, mo, pc);
    }
    auto t1 = std::chrono::steady_clock::now();
    ok = true;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / kN;
}

// Re-blocked: the module accumulates N samples, runs the graph once, drains.
// This is what Surge XT Rack does (8 samples), and it amortizes the fixed
// per-call cost -- at the price of N samples of latency.
static double reblocked(int nodes, int block, const std::vector<float>& in,
                        std::vector<float>& out, bool& ok) {
    SignalGraph g;
    const auto ni = g.add_input_node(1, "In");
    const auto no = g.add_output_node(1, "Out");
    auto prev = ni;
    for (int i = 0; i < nodes; ++i) {
        const auto gn = g.add_gain_node("G");
        g.connect(prev, 0, gn, 0);
        g.set_node_gain(gn, 0.9f);
        prev = gn;
    }
    g.connect(prev, 0, no, 0);
    if (!g.prepare(kSr, block)) { ok = false; return 0; }
    auto r = bake(g);
    if (!r.accepted || !r.processor) { ok = false; return 0; }
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = kSr; ctx.max_buffer_size = block;
    ctx.input_channels = 1; ctx.output_channels = 1;
    r.processor->prepare(ctx);

    std::vector<float> inbuf(block), outbuf(block);
    pulp::midi::MidiBuffer mi, mo;
    pulp::format::ProcessContext pc; pc.sample_rate = kSr;
    int fill = 0, drain = block;
    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kN; ++i) {
        // per-sample in, per-sample out -- exactly a Rack module's contract
        inbuf[fill++] = in[i];
        out[i] = outbuf[drain < block ? drain++ : block - 1];
        if (fill == block) {
            const float* ins[1] = {inbuf.data()}; float* outs[1] = {outbuf.data()};
            pulp::audio::BufferView<const float> iv(ins, 1, (std::uint32_t)block);
            pulp::audio::BufferView<float> ov(outs, 1, (std::uint32_t)block);
            mi.clear(); mo.clear();
            r.processor->process(ov, iv, mi, mo, pc);
            fill = 0; drain = 0;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    ok = true;
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / kN;
}

int main() {
    auto in = tone();
    std::vector<float> out(kN);
    // One core at 48kHz = 20833 ns per sample of budget.
    const double budget = 1e9 / kSr;

    printf("per-sample cost, 48kHz (1 core = %.0f ns/sample)\n\n", budget);
    const double hw = hand_written(in, out);
    printf("  hand-written SVF filter      %7.1f ns  = %5.2f%% of a core\n",
           hw, 100.0 * hw / budget);

    for (int n : {1, 2, 4, 8}) {
        bool ok = false;
        const double bg = baked_graph(n, in, out, ok);
        if (!ok) { printf("  baked graph %d node(s): REFUSED\n", n); continue; }
        printf("  baked graph, %d gain node%s  %7.1f ns  = %5.2f%% of a core   (%.1fx hand-written)\n",
               n, n == 1 ? " " : "s", bg, 100.0 * bg / budget, bg / hw);
    }
    printf("\n  re-blocked inside the module (4-node graph, per-sample in/out):\n");
    double best = 0;
    for (int b : {4, 8, 16, 32, 64}) {
        bool ok = false;
        const double rb = reblocked(4, b, in, out, ok);
        if (!ok) { printf("    block %-3d REFUSED\n", b); continue; }
        if (b == 8) best = rb;
        printf("    block %-3d %7.1f ns  = %5.2f%% of a core   (%.1fx hand-written, "
               "%.1f ms latency)\n", b, rb, 100.0 * rb / budget, rb / hw,
               1000.0 * b / kSr);
    }
    printf("\n  20-module rack: hand-written %.1f%%  |  block-1 graphs %.1f%%  |  "
           "block-8 graphs %.1f%%\n",
           20 * 100.0 * hw / budget, 20 * 0.89, 20 * 100.0 * best / budget);
    return 0;
}

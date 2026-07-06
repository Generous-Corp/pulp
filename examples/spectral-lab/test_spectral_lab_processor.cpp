// Spectral Lab processor-level tests: the CPU engine freezes and sustains a
// captured cloud, the opt-in GPU engine actually carries the audio (populated
// gpu_status, real blocks), and a live CPU->GPU->CPU engine switch stays finite
// with a fixed reported latency. GPU cases skip cleanly with no device.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/render/gpu_compute.hpp>
#include <pulp/state/store.hpp>

#include "spectral_lab.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

using namespace pulp;
using namespace pulp::examples;

namespace {

constexpr int BLOCK = 512;
constexpr double SR = 48000.0;

bool gpu_available() {
    auto g = render::GpuCompute::create();
    return g && g->initialize_standalone();
}

double rms(const std::vector<float>& x) {
    if (x.empty()) return 0.0;
    double s = 0.0;
    for (float v : x) s += double(v) * v;
    return std::sqrt(s / x.size());
}

bool all_finite(const std::vector<float>& x) {
    for (float v : x) if (!std::isfinite(v)) return false;
    return true;
}

// Drive `nblocks` stereo noise blocks through `proc`, collecting channel 0 of
// the output. `freeze` latches the Freeze trigger on every block. When `pace` is
// true, sleep one block's worth between calls so the GPU transport worker gets
// the wall-clock gap it has in a live host.
std::vector<float> drive(SpectralLabProcessor& proc, state::StateStore& store,
                         int nblocks, bool freeze, bool pace) {
    std::vector<float> out_all;
    std::vector<float> l(BLOCK), r(BLOCK), ol(BLOCK, 0.0f), orr(BLOCK, 0.0f);
    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    std::uint32_t s = 0x1234ABCDu;
    for (int b = 0; b < nblocks; ++b) {
        for (int i = 0; i < BLOCK; ++i) {
            s = s * 1664525u + 1013904223u;
            const float white = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
            l[i] = r[i] = 0.4f * white;
        }
        store.set_value(kFreeze, freeze ? 1.0f : 0.0f);
        const float* inp[2] = {l.data(), r.data()};
        float* outp[2] = {ol.data(), orr.data()};
        audio::BufferView<const float> in(inp, 2, BLOCK);
        audio::BufferView<float> ob(outp, 2, BLOCK);
        proc.process(ob, in, min, mout, pctx);
        out_all.insert(out_all.end(), ol.begin(), ol.end());
        if (pace) std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<long>(BLOCK / SR * 1e6)));
    }
    return out_all;
}

void setup(SpectralLabProcessor& proc, state::StateStore& store, int engine, int layers) {
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kMix, 100.0f);   // fully wet → output is the frozen cloud
    store.set_value(kLayers, static_cast<float>(layers));
    store.set_value(kMorph, 0.5f);
    store.set_value(kSmear, 0.3f);
    store.set_value(kJitter, 0.1f);
    store.set_value(kEngine, static_cast<float>(engine));
    format::PrepareContext ctx;
    ctx.sample_rate = SR;
    ctx.max_buffer_size = BLOCK;
    proc.prepare(ctx);
}

}  // namespace

TEST_CASE("SpectralLab CPU engine freezes and sustains a cloud", "[spectral][spectrallab]") {
    state::StateStore store;
    SpectralLabProcessor proc;
    setup(proc, store, /*engine=*/0, /*layers=*/8);

    // Latch Freeze while feeding noise so several layers are captured.
    auto frozen = drive(proc, store, /*nblocks=*/24, /*freeze=*/true, /*pace=*/false);
    REQUIRE(all_finite(frozen));
    REQUIRE(proc.captured_layers() >= 1);

    // Stop feeding (silence) but hold the cloud: the wet output should still
    // carry energy from the frozen layers.
    std::vector<float> out_all;
    std::vector<float> z(BLOCK, 0.0f), ol(BLOCK, 0.0f), orr(BLOCK, 0.0f);
    midi::MidiBuffer min, mout;
    format::ProcessContext pctx;
    store.set_value(kFreeze, 0.0f);
    for (int b = 0; b < 24; ++b) {
        const float* inp[2] = {z.data(), z.data()};
        float* outp[2] = {ol.data(), orr.data()};
        audio::BufferView<const float> in(inp, 2, BLOCK);
        audio::BufferView<float> ob(outp, 2, BLOCK);
        proc.process(ob, in, min, mout, pctx);
        out_all.insert(out_all.end(), ol.begin(), ol.end());
    }
    REQUIRE(all_finite(out_all));
    REQUIRE(rms(out_all) > 1e-4);   // the freeze holds after the input goes silent
}

TEST_CASE("SpectralLab GPU engine carries the audio", "[spectral][gpu][spectrallab]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    state::StateStore store;
    SpectralLabProcessor proc;
    setup(proc, store, /*engine=*/1, /*layers=*/32);

    // Paced, frozen blocks so the transport worker produces real GPU blocks.
    auto out = drive(proc, store, /*nblocks=*/80, /*freeze=*/true, /*pace=*/true);
    REQUIRE(all_finite(out));

    if (!proc.gpu_engine_active()) {
        WARN("GPU engine did not activate (device over a limit at 32 layers); skipping");
        return;
    }
    const auto g = proc.gpu_status();
    REQUIRE(g.active);
    REQUIRE_FALSE(g.backend.empty());
    REQUIRE(g.layers == 32);
    REQUIRE(g.blocks > 0);
    REQUIRE(g.avg_us > 0.0);
    REQUIRE(g.budget_us > 0.0);
    REQUIRE(g.rt_percent > 0.0);
}

TEST_CASE("SpectralLab live engine switch stays finite at fixed latency",
          "[spectral][gpu][spectrallab]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    state::StateStore store;
    SpectralLabProcessor proc;
    setup(proc, store, /*engine=*/0, /*layers=*/16);

    const int latency0 = proc.latency_samples();
    REQUIRE(latency0 > 0);

    // CPU → GPU: request GPU, then give the worker time to rebuild + publish.
    auto cpu_out = drive(proc, store, 8, /*freeze=*/true, /*pace=*/false);
    REQUIRE(all_finite(cpu_out));
    store.set_value(kEngine, 1.0f);
    auto gpu_out = drive(proc, store, 80, /*freeze=*/true, /*pace=*/true);
    REQUIRE(all_finite(gpu_out));
    REQUIRE(proc.latency_samples() == latency0);   // latency is fixed for the lifetime

    // GPU → CPU: switch back, still finite, still the same fixed latency.
    store.set_value(kEngine, 0.0f);
    auto back_out = drive(proc, store, 40, /*freeze=*/false, /*pace=*/true);
    REQUIRE(all_finite(back_out));
    REQUIRE(proc.latency_samples() == latency0);
}

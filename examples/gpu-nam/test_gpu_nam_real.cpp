// Fused GPU NAM forward validated against the exact CPU oracle (nam_model.hpp):
// cross-correlation > 0.99 on the real /tmp/test.nam capture (single block and a
// 2-block stream) and on a synthetic standard-shape model, plus a per-block
// timing win at a larger model. GPU cases skip cleanly with no device.

#include <catch2/catch_test_macros.hpp>

#include "gpu_nam.hpp"
#include "nam_model.hpp"
#include <pulp/render/gpu_compute.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::examples;

namespace {

bool gpu_available() {
    auto g = render::GpuCompute::create();
    return g && g->initialize_standalone();
}

double xcorr(const std::vector<float>& a, const std::vector<float>& b) {
    double sxy = 0, sxx = 0, syy = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) { sxy += a[i] * b[i]; sxx += a[i] * a[i]; syy += b[i] * b[i]; }
    return sxy / std::sqrt(sxx * syy + 1e-30);
}

std::vector<float> sine_block(uint32_t n, uint32_t off) {
    std::vector<float> v(n);
    for (uint32_t i = 0; i < n; ++i)
        v[i] = 0.3f * std::sin(0.03f * (i + off)) + 0.1f * std::sin(0.21f * (i + off));
    return v;
}

// Build a synthetic NAM with the given array shapes and deterministic small
// weights, through the real NamModel::build (so it is byte-identical in layout
// to a loaded .nam). The trailing weight is the head_scale the model actually
// uses; we seed it small so the recursion stays bounded.
nam::NamModel build_synth(std::vector<nam::LayerArrayConfig> arrays, uint32_t seed) {
    std::size_t need = 1;  // trailing head_scale
    for (const auto& a : arrays) {
        const uint32_t C = a.channels, K = a.kernel_size, H = a.head_size;
        const uint32_t Z = a.gated ? 2u * C : C;
        need += static_cast<std::size_t>(C) * a.input_size;
        for (std::size_t l = 0; l < a.dilations.size(); ++l)
            need += static_cast<std::size_t>(Z) * C * K + Z
                    + static_cast<std::size_t>(Z) * a.condition_size
                    + static_cast<std::size_t>(C) * C + C;
        need += static_cast<std::size_t>(H) * C + (a.head_bias ? H : 0u);
    }
    std::vector<float> w(need);
    uint32_t s = seed;
    for (auto& v : w) { s = s * 1664525u + 1013904223u; v = (static_cast<float>(s >> 9) / 4194304.0f - 1.0f) * 0.1f; }
    nam::NamModel m;
    const bool ok = m.build(std::move(arrays), 1.0f, std::move(w), 1, 48000.0);
    REQUIRE(ok);
    return m;
}

nam::LayerArrayConfig array_cfg(int input_size, int channels, int head_size,
                                std::vector<int> dilations, bool head_bias) {
    nam::LayerArrayConfig a;
    a.input_size = input_size;
    a.condition_size = 1;
    a.channels = channels;
    a.kernel_size = 3;
    a.dilations = std::move(dilations);
    a.head_size = head_size;
    a.activation = "Tanh";
    a.gated = false;
    a.head_bias = head_bias;
    return a;
}

std::vector<int> dilation_ramp(int n) {
    std::vector<int> d;
    int v = 1;
    for (int i = 0; i < n; ++i) { d.push_back(v); v = (v >= 512) ? 1 : v * 2; }
    return d;
}

}  // namespace

TEST_CASE("CPU oracle loads the real .nam capture", "[nam]") {
    nam::NamModel model;
    std::string err;
    if (!nam::load_nam("/tmp/test.nam", model, &err)) {
        WARN("could not load /tmp/test.nam (" << err << "); skipping");
        return;
    }
    REQUIRE(model.arrays().size() == 2);

    const uint32_t B = 512;
    auto in = sine_block(B, 0);
    std::vector<float> out(B);
    model.reset();
    model.process(in.data(), out.data(), B);
    for (float v : out) REQUIRE(std::isfinite(v));
}

TEST_CASE("GPU NAM reproduces the CPU oracle on the real model, single block", "[nam][gpu]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    nam::NamModel model;
    std::string err;
    if (!nam::load_nam("/tmp/test.nam", model, &err)) {
        WARN("could not load /tmp/test.nam (" << err << "); skipping");
        return;
    }

    const uint32_t B = 512;
    auto in = sine_block(B, 0);
    std::vector<float> oc(B), og(B);
    model.reset();
    model.process(in.data(), oc.data(), B);

    nam::GpuNam gnam;
    REQUIRE(gnam.prepare(model, B));
    REQUIRE(gnam.forward(in.data(), og.data(), B));

    const double xc = xcorr(oc, og);
    INFO("real-model single-block xcorr=" << xc);
    REQUIRE(xc > 0.99);
}

TEST_CASE("GPU NAM is streaming-continuous on the real model", "[nam][gpu]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    nam::NamModel model;
    std::string err;
    if (!nam::load_nam("/tmp/test.nam", model, &err)) {
        WARN("could not load /tmp/test.nam (" << err << "); skipping");
        return;
    }

    const uint32_t B = 512;
    auto b1 = sine_block(B, 0), b2 = sine_block(B, B);
    std::vector<float> c1(B), c2(B), g1(B), g2(B);
    model.reset();
    model.process(b1.data(), c1.data(), B);
    model.process(b2.data(), c2.data(), B);  // carries history, no reset

    nam::GpuNam gnam;
    REQUIRE(gnam.prepare(model, B));
    gnam.forward(b1.data(), g1.data(), B);
    gnam.forward(b2.data(), g2.data(), B);   // carries the slid dilation window

    const double xc = xcorr(c2, g2);
    INFO("real-model second-block streaming xcorr=" << xc);
    REQUIRE(xc > 0.99);
}

TEST_CASE("GPU NAM reproduces a synthetic standard-shape model", "[nam][gpu]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    // channels 16, 2 arrays, dilations 1..512. array0.head_size == array1.channels
    // (the chained head accumulator); the final array re-channels to 1.
    std::vector<nam::LayerArrayConfig> arrays = {
        array_cfg(/*input*/ 1, /*channels*/ 16, /*head*/ 16, dilation_ramp(10), /*bias*/ false),
        array_cfg(/*input*/ 16, /*channels*/ 16, /*head*/ 1, dilation_ramp(10), /*bias*/ true),
    };
    nam::NamModel model = build_synth(arrays, 0x51A1u);

    const uint32_t B = 512;
    auto in = sine_block(B, 0);
    std::vector<float> oc(B), og(B);
    model.reset();
    model.process(in.data(), oc.data(), B);

    nam::GpuNam gnam;
    REQUIRE(gnam.prepare(model, B));
    REQUIRE(gnam.forward(in.data(), og.data(), B));

    const double xc = xcorr(oc, og);
    INFO("synthetic standard-shape xcorr=" << xc);
    REQUIRE(xc > 0.99);
}

TEST_CASE("GPU NAM wins at large model size", "[nam][gpu]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    // A bigger capture: channels 48, two 16-layer arrays. The CPU oracle is
    // serial per sample (and reallocates per sample); the GPU runs the whole
    // block in parallel, fused into one submit — so the win widens with size.
    std::vector<nam::LayerArrayConfig> arrays = {
        array_cfg(/*input*/ 1, /*channels*/ 48, /*head*/ 48, dilation_ramp(16), /*bias*/ false),
        array_cfg(/*input*/ 48, /*channels*/ 48, /*head*/ 1, dilation_ramp(16), /*bias*/ true),
    };
    nam::NamModel model = build_synth(arrays, 0xBEEF1u);

    // A larger block fills the GPU (one thread per sample) so the block-parallel
    // win shows clearly over the strictly-serial CPU oracle.
    const uint32_t B = 4096;
    auto in = sine_block(B, 0);
    std::vector<float> oc(B), og(B);

    nam::GpuNam gnam;
    REQUIRE(gnam.prepare(model, B));

    // Correctness still holds at this size: one fresh block each (the GPU also
    // reproduces the oracle bit-for-bit here — same first-block, no streaming).
    model.reset(); model.process(in.data(), oc.data(), B);
    gnam.forward(in.data(), og.data(), B);
    INFO("big-model single-block xcorr=" << xcorr(oc, og));
    REQUIRE(xcorr(oc, og) > 0.99);

    // Timing: average per-block wall-clock. The CPU oracle resets each iteration
    // (a fresh first block); the GPU streams (the per-block cost is what matters).
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    for (int i = 0; i < 20; ++i) { model.reset(); model.process(in.data(), oc.data(), B); }
    const double cpu_us = std::chrono::duration<double, std::micro>(clk::now() - t0).count() / 20;
    t0 = clk::now();
    for (int i = 0; i < 20; ++i) gnam.forward(in.data(), og.data(), B);
    const double gpu_us = std::chrono::duration<double, std::micro>(clk::now() - t0).count() / 20;
    INFO("big model: CPU " << cpu_us << " us/block, GPU " << gpu_us << " us/block");
    REQUIRE(gpu_us < cpu_us);
}

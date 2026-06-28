// GPU NAM tests: the fused block-parallel GPU WaveNet reproduces the CPU
// reference (single block and streaming across blocks) and wins at scale. GPU
// cases skip cleanly with no device.

#include <catch2/catch_test_macros.hpp>

#include "wavenet.hpp"
#include <pulp/render/gpu_compute.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp;

namespace {

bool gpu_available() {
    auto g = render::GpuCompute::create();
    return g && g->initialize_standalone();
}

double xcorr(const std::vector<float>& a, const std::vector<float>& b) {
    double sxy = 0, sxx = 0, syy = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) { sxy += a[i]*b[i]; sxx += a[i]*a[i]; syy += b[i]*b[i]; }
    return sxy / std::sqrt(sxx * syy + 1e-30);
}

examples::WaveNetConfig standard_config() {
    examples::WaveNetConfig cfg;
    cfg.channels = 16; cfg.kernel = 3;
    uint32_t d = 1;
    for (int l = 0; l < 16; ++l) { cfg.dilations.push_back(d); d = (d >= 512) ? 1 : d * 2; }
    return cfg;
}

std::vector<float> sine_block(uint32_t n, uint32_t off) {
    std::vector<float> v(n);
    for (uint32_t i = 0; i < n; ++i)
        v[i] = 0.2f * std::sin(0.03f * (i + off)) + 0.05f * std::sin(0.21f * (i + off));
    return v;
}

}  // namespace

TEST_CASE("CPU WaveNet reference is causal and finite", "[nam]") {
    auto cfg = standard_config();
    auto w = examples::make_synthetic_wavenet(cfg);
    examples::WaveNetCpu net;
    REQUIRE(net.load(cfg, w));

    const uint32_t B = 512;
    auto in = sine_block(B, 0);
    std::vector<float> out(B);
    net.reset(); net.process(in.data(), out.data(), B);
    for (float v : out) REQUIRE(std::isfinite(v));

    // A change at sample 300 must not affect any earlier output (causality).
    auto in2 = in; in2[300] += 1.0f;
    std::vector<float> out2(B);
    net.reset(); net.process(in.data(), out.data(), B);
    net.reset(); net.process(in2.data(), out2.data(), B);
    int first = -1;
    for (uint32_t i = 0; i < B; ++i) if (std::fabs(out[i]-out2[i]) > 1e-7f) { first = int(i); break; }
    REQUIRE(first >= 300);
}

TEST_CASE("GPU conv-stack reproduces the CPU WaveNet, single block", "[nam][gpu]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    auto cfg = standard_config();
    auto w = examples::make_synthetic_wavenet(cfg);
    examples::WaveNetCpu net; REQUIRE(net.load(cfg, w));
    auto gpu = render::GpuCompute::create(); REQUIRE(gpu->initialize_standalone());

    const uint32_t B = 512;
    auto in = sine_block(B, 0);
    std::vector<float> oc(B), og(B);
    net.reset(); net.process(in.data(), oc.data(), B);
    REQUIRE(gpu->prepare_conv_stack(cfg.channels, cfg.kernel, cfg.dilations.data(),
                                    cfg.num_layers(), w.data(),
                                    static_cast<uint32_t>(w.size()), B, cfg.head_scale));
    REQUIRE(gpu->conv_stack_forward(in.data(), og.data(), B));
    INFO("single-block xcorr=" << xcorr(oc, og));
    REQUIRE(xcorr(oc, og) > 0.99);
}

TEST_CASE("GPU conv-stack is streaming-continuous across blocks", "[nam][gpu]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    auto cfg = standard_config();
    auto w = examples::make_synthetic_wavenet(cfg);
    examples::WaveNetCpu net; REQUIRE(net.load(cfg, w));
    auto gpu = render::GpuCompute::create(); REQUIRE(gpu->initialize_standalone());

    const uint32_t B = 512;
    auto b1 = sine_block(B, 0), b2 = sine_block(B, B);
    std::vector<float> c1(B), c2(B), g1(B), g2(B);
    net.reset();
    net.process(b1.data(), c1.data(), B);
    net.process(b2.data(), c2.data(), B);     // carries history, no reset
    REQUIRE(gpu->prepare_conv_stack(cfg.channels, cfg.kernel, cfg.dilations.data(),
                                    cfg.num_layers(), w.data(),
                                    static_cast<uint32_t>(w.size()), B, cfg.head_scale));
    gpu->conv_stack_forward(b1.data(), g1.data(), B);
    gpu->conv_stack_forward(b2.data(), g2.data(), B);   // carries the slid window
    INFO("second-block streaming xcorr=" << xcorr(c2, g2));
    REQUIRE(xcorr(c2, g2) > 0.99);   // cross-block context, not a per-block restart
}

TEST_CASE("GPU conv-stack wins at large model size", "[nam][gpu]") {
    if (!gpu_available()) { WARN("no GPU device; skipping"); return; }
    examples::WaveNetConfig cfg;
    cfg.channels = 32; cfg.kernel = 3;
    uint32_t d = 1;
    for (int l = 0; l < 24; ++l) { cfg.dilations.push_back(d); d = (d >= 512) ? 1 : d * 2; }
    auto w = examples::make_synthetic_wavenet(cfg);
    examples::WaveNetCpu net; REQUIRE(net.load(cfg, w));
    auto gpu = render::GpuCompute::create(); REQUIRE(gpu->initialize_standalone());

    const uint32_t B = 512;
    auto in = sine_block(B, 0);
    std::vector<float> oc(B), og(B);
    REQUIRE(gpu->prepare_conv_stack(cfg.channels, cfg.kernel, cfg.dilations.data(),
                                    cfg.num_layers(), w.data(),
                                    static_cast<uint32_t>(w.size()), B, cfg.head_scale));
    for (int i = 0; i < 3; ++i) { net.reset(); net.process(in.data(), oc.data(), B);
                                  gpu->conv_stack_forward(in.data(), og.data(), B); }
    using clk = std::chrono::steady_clock;
    auto t0 = clk::now();
    for (int i = 0; i < 20; ++i) { net.reset(); net.process(in.data(), oc.data(), B); }
    const double cpu_us = std::chrono::duration<double, std::micro>(clk::now() - t0).count() / 20;
    t0 = clk::now();
    for (int i = 0; i < 20; ++i) gpu->conv_stack_forward(in.data(), og.data(), B);
    const double gpu_us = std::chrono::duration<double, std::micro>(clk::now() - t0).count() / 20;
    INFO("big model: CPU " << cpu_us << " us, GPU " << gpu_us << " us");
    REQUIRE(gpu_us < cpu_us);
}

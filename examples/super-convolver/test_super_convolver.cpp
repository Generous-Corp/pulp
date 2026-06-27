#include <catch2/catch_test_macros.hpp>

#include <pulp/format/headless.hpp>

#include "super_convolver.hpp"

#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using namespace pulp::examples;

namespace {

// Render `nblocks` blocks of `block` samples through the host, with `in_ch0`
// supplying channel 0 of the FIRST block (rest zero) — i.e. an impulse/probe —
// and collect channel 0 of the output.
std::vector<float> render(pulp::format::HeadlessHost& host, std::size_t block,
                          int nblocks, const std::vector<float>& first_block) {
    std::vector<float> out_all;
    std::vector<float> in_l(block, 0.0f), in_r(block, 0.0f);
    std::vector<float> out_l(block, 0.0f), out_r(block, 0.0f);
    for (int b = 0; b < nblocks; ++b) {
        std::fill(in_l.begin(), in_l.end(), 0.0f);
        std::fill(in_r.begin(), in_r.end(), 0.0f);
        if (b == 0)
            for (std::size_t i = 0; i < block && i < first_block.size(); ++i) {
                in_l[i] = first_block[i];
                in_r[i] = first_block[i];
            }
        const float* in_ptrs[2] = {in_l.data(), in_r.data()};
        float* out_ptrs[2] = {out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> in_view(in_ptrs, 2, block);
        pulp::audio::BufferView<float> out_view(out_ptrs, 2, block);
        host.process(out_view, in_view);
        out_all.insert(out_all.end(), out_l.begin(), out_l.end());
    }
    return out_all;
}

}  // namespace

TEST_CASE("SuperConvolver impulse response matches the IR", "[golden][superconvolver]") {
    constexpr std::size_t BLOCK = 512;
    constexpr double SR = 48000.0;
    constexpr float SIZE = 0.05f;  // short IR for a fast, exact test
    const std::size_t len = static_cast<std::size_t>(SIZE * SR);

    pulp::format::HeadlessHost host(create_super_convolver);
    // Restore state, THEN prepare — the processor builds its IR at prepare()
    // from the current Size (runtime Size changes go through the background IR
    // swapper, not an in-process realloc), so Size must be set before prepare.
    host.state().set_value(kSize, SIZE);
    host.state().set_value(kMix, 100.0f);   // fully wet → output == convolution
    host.state().set_value(kGain, 0.0f);
    host.state().set_value(kBypass, 0.0f);
    host.prepare(SR, static_cast<int>(BLOCK));

    const std::vector<float> ir = make_reverb_ir(len);  // same deterministic IR
    std::vector<float> impulse(BLOCK, 0.0f);
    impulse[0] = 1.0f;

    const int nblocks = static_cast<int>((len + BLOCK) / BLOCK) + 2;
    const std::vector<float> out = render(host, BLOCK, nblocks, impulse);

    // The convolver may add block latency; find the onset (the IR's peak is its
    // first sample = 1.0) and align.
    std::size_t lag = 0;
    float peak = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (std::abs(out[i]) > peak) { peak = std::abs(out[i]); lag = i; }
    REQUIRE(peak > 0.9f);  // the IR's unit onset survived

    // Validate the *shape* matches the IR via normalized cross-correlation and
    // relative RMS error — the robust golden for a convolution path, immune to
    // the per-sample f32 FFT roundoff that broadband noise accumulates.
    double sxy = 0, sxx = 0, syy = 0, err2 = 0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < len && lag + i < out.size(); ++i) {
        const double x = out[lag + i], y = ir[i];
        sxy += x * y; sxx += x * x; syy += y * y; err2 += (x - y) * (x - y); ++n;
    }
    const double corr = sxy / std::sqrt(sxx * syy + 1e-30);
    const double rel_rms = std::sqrt(err2 / n) / std::sqrt(syy / n);
    INFO("corr=" << corr << " rel_rms=" << rel_rms);
    REQUIRE(corr > 0.9999);      // output is the IR, sample-for-sample
    REQUIRE(rel_rms < 0.01);     // within 1% RMS of the reference IR (f32 FFT)
}

TEST_CASE("SuperConvolver Size knob swaps the IR live", "[golden][superconvolver]") {
    constexpr std::size_t BLOCK = 512;
    constexpr double SR = 48000.0;
    pulp::format::HeadlessHost host(create_super_convolver);
    host.state().set_value(kSize, 0.05f);   // start small
    host.state().set_value(kMix, 100.0f);
    host.state().set_value(kGain, 0.0f);
    host.state().set_value(kBypass, 0.0f);
    host.prepare(SR, static_cast<int>(BLOCK));

    // Establish the small IR is live: a fresh impulse decays to ~0 well before
    // the larger IR would, so we can detect the swap through audio alone.
    const std::size_t small_len = static_cast<std::size_t>(0.05f * SR);
    const std::size_t want_len = static_cast<std::size_t>(0.30f * SR);

    // Request a much larger IR and pump silent blocks so the background worker
    // rebuilds it and the audio thread swaps it in at a block boundary. The
    // rebuild is sub-millisecond; this warm-up is generously over-provisioned.
    host.state().set_value(kSize, 0.30f);
    std::vector<float> silence(BLOCK, 0.0f);
    for (int tick = 0; tick < 80; ++tick) {                   // ~400ms of slack
        render(host, BLOCK, 1, silence);                      // advances + swaps
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Probe with a fresh impulse and confirm the convolution is now the NEW,
    // larger IR — correlation over the full new length only passes post-swap.
    const std::vector<float> new_ir = make_reverb_ir(want_len);
    std::vector<float> impulse(BLOCK, 0.0f);
    impulse[0] = 1.0f;
    const int nblocks = static_cast<int>((want_len + BLOCK) / BLOCK) + 2;
    const auto out = render(host, BLOCK, nblocks, impulse);

    std::size_t lag = 0; float peak = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (std::abs(out[i]) > peak) { peak = std::abs(out[i]); lag = i; }
    double sxy = 0, sxx = 0, syy = 0;
    for (std::size_t i = 0; i < want_len && lag + i < out.size(); ++i) {
        const double x = out[lag + i], y = new_ir[i];
        sxy += x * y; sxx += x * x; syy += y * y;
    }
    const double corr = sxy / std::sqrt(sxx * syy + 1e-30);
    INFO("post-swap corr=" << corr);
    REQUIRE(corr > 0.9999);   // the live-swapped IR is the new, larger IR

    // Tail energy past the OLD IR's length proves the new IR is in play (the
    // 0.05s IR is silent here; the 0.30s IR is not).
    double tail = 0.0;
    for (std::size_t i = small_len + BLOCK; i < want_len && lag + i < out.size(); ++i)
        tail += static_cast<double>(out[lag + i]) * out[lag + i];
    REQUIRE(tail > 1e-3);
}

TEST_CASE("SuperConvolver mix=0 and bypass pass the dry signal", "[golden][superconvolver]") {
    // The re-blocking FIFO delays the path by kInternalBlock; the dry path is
    // delayed to match (so dry/wet stay phase-aligned, reported via
    // latency_samples()). With BLOCK == kInternalBlock the dry probe re-emerges,
    // sample-exact, in the NEXT block.
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    pulp::format::HeadlessHost host(create_super_convolver);
    host.prepare(48000.0, static_cast<int>(BLOCK));
    host.state().set_value(kSize, 0.1f);
    host.state().set_value(kGain, 0.0f);

    std::vector<float> probe(BLOCK);
    for (std::size_t i = 0; i < BLOCK; ++i) probe[i] = std::sin(0.05f * i);

    SECTION("mix=0 is dry (delayed by the reported latency)") {
        host.state().set_value(kBypass, 0.0f);
        host.state().set_value(kMix, 0.0f);
        const auto out = render(host, BLOCK, 2, probe);   // probe in block 0, silence block 1
        for (std::size_t i = 0; i < BLOCK; ++i)
            REQUIRE(std::abs(out[BLOCK + i] - probe[i]) < 1e-6f);
    }
    SECTION("bypass is dry (delayed by the reported latency)") {
        host.state().set_value(kBypass, 1.0f);
        host.state().set_value(kMix, 100.0f);
        const auto out = render(host, BLOCK, 2, probe);
        for (std::size_t i = 0; i < BLOCK; ++i)
            REQUIRE(std::abs(out[BLOCK + i] - probe[i]) < 1e-6f);
    }
}

TEST_CASE("SuperConvolver convolves correctly under variable host blocks",
          "[golden][superconvolver]") {
    // Regression guard: the convolver needs fixed-size blocks, but real hosts
    // (and the standalone, which floors max_buffer_size well above the device
    // pull) deliver smaller, varying blocks. Prepare for a large max block, then
    // drive irregular smaller blocks and confirm the reverb still reproduces the
    // IR (peak-aligned), proving the internal re-blocking FIFO works.
    constexpr double SR = 48000.0;
    constexpr float SIZE = 0.05f;
    const std::size_t len = static_cast<std::size_t>(SIZE * SR);

    pulp::format::HeadlessHost host(create_super_convolver);
    host.state().set_value(kSize, SIZE);
    host.state().set_value(kMix, 100.0f);
    host.state().set_value(kGain, 0.0f);
    host.state().set_value(kBypass, 0.0f);
    host.prepare(SR, 4096);   // host advertises a large max block...

    // ...but feeds these irregular, smaller blocks.
    const std::vector<std::size_t> blocks = {1, 127, 200, 64, 333, 512, 96, 256, 500, 333};
    std::size_t total = 0; for (auto b : blocks) total += b;
    while (total < len + 4096) { for (auto b : blocks) total += b; }  // enough to flush the tail

    const std::vector<float> ir = make_reverb_ir(len);
    std::vector<float> out_all;
    bool impulse_sent = false;
    std::size_t produced = 0;
    while (produced < total) {
        for (std::size_t blk : blocks) {
            std::vector<float> in_l(blk, 0.0f), in_r(blk, 0.0f), o_l(blk, 0.0f), o_r(blk, 0.0f);
            if (!impulse_sent) { in_l[0] = 1.0f; in_r[0] = 1.0f; impulse_sent = true; }
            const float* ip[2] = {in_l.data(), in_r.data()};
            float* op[2] = {o_l.data(), o_r.data()};
            pulp::audio::BufferView<const float> iv(ip, 2, blk);
            pulp::audio::BufferView<float> ov(op, 2, blk);
            host.process(ov, iv);
            out_all.insert(out_all.end(), o_l.begin(), o_l.end());
            produced += blk;
        }
    }

    std::size_t lag = 0; float peak = 0.0f;
    for (std::size_t i = 0; i < out_all.size(); ++i)
        if (std::abs(out_all[i]) > peak) { peak = std::abs(out_all[i]); lag = i; }
    REQUIRE(peak > 0.9f);   // NOT dry-passed: a real convolved onset survived

    double sxy = 0, sxx = 0, syy = 0;
    for (std::size_t i = 0; i < len && lag + i < out_all.size(); ++i) {
        const double x = out_all[lag + i], y = ir[i];
        sxy += x * y; sxx += x * x; syy += y * y;
    }
    const double corr = sxy / std::sqrt(sxx * syy + 1e-30);
    INFO("variable-block corr=" << corr);
    REQUIRE(corr > 0.9999);   // reverb is the IR even though host blocks != prepared block
}

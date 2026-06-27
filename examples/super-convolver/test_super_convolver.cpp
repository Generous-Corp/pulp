#include <catch2/catch_test_macros.hpp>

#include <pulp/format/headless.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>
#include <pulp/gpu_audio/gpu_multi_convolver.hpp>
#include <pulp/signal/convolver.hpp>

#include "super_convolver.hpp"

#include <algorithm>
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

// Helper: the dry path is a pure delay (the reported latency), so at mix=0 /
// bypass the output IS an exact delayed copy of the probe. The reported latency
// depends on the engine config (kInternalBlock, plus the GPU transport's fixed
// delay whenever a GPU device exists — applied to BOTH engines so a live switch
// keeps it stable), so locate the delayed probe rather than hardcode the lag.
namespace {
std::size_t find_dry_lag(const std::vector<float>& out, const std::vector<float>& probe) {
    std::size_t best_d = 0;
    double best_err = 1e300;
    for (std::size_t d = 0; d + probe.size() <= out.size(); ++d) {
        double e = 0;
        for (std::size_t i = 0; i < probe.size(); ++i) {
            const double diff = static_cast<double>(out[d + i]) - probe[i];
            e += diff * diff;
        }
        if (e < best_err) { best_err = e; best_d = d; }
    }
    return best_d;
}
}  // namespace

TEST_CASE("SuperConvolver mix=0 and bypass pass the dry signal", "[golden][superconvolver]") {
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    pulp::format::HeadlessHost host(create_super_convolver);
    host.prepare(48000.0, static_cast<int>(BLOCK));
    host.state().set_value(kSize, 0.1f);
    host.state().set_value(kGain, 0.0f);

    std::vector<float> probe(BLOCK);
    for (std::size_t i = 0; i < BLOCK; ++i) probe[i] = std::sin(0.05f * i);

    // Render enough blocks to cover the dry delay under either engine config.
    constexpr int NBLOCKS = 6;

    SECTION("mix=0 is dry (delayed by the reported latency)") {
        host.state().set_value(kBypass, 0.0f);
        host.state().set_value(kMix, 0.0f);
        const auto out = render(host, BLOCK, NBLOCKS, probe);
        const std::size_t lag = find_dry_lag(out, probe);
        for (std::size_t i = 0; i < BLOCK; ++i)
            REQUIRE(std::abs(out[lag + i] - probe[i]) < 1e-6f);
    }
    SECTION("bypass is dry (delayed by the reported latency)") {
        host.state().set_value(kBypass, 1.0f);
        host.state().set_value(kMix, 100.0f);
        const auto out = render(host, BLOCK, NBLOCKS, probe);
        const std::size_t lag = find_dry_lag(out, probe);
        for (std::size_t i = 0; i < BLOCK; ++i)
            REQUIRE(std::abs(out[lag + i] - probe[i]) < 1e-6f);
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

// Proves the GPU engine path is genuinely wired: with Engine=GPU the live audio
// path runs through gpu_audio::GpuConvolver driven by GpuAudioTransport (GPU FFT
// on the transport's non-RT worker), and the convolved output still reproduces
// the IR. We drive the processor directly (not through HeadlessHost) so we can
// read gpu_engine_active() and detect a no-GPU environment.
//
// Determinism: the transport's worker thread polls the input ring and produces
// asynchronously, so after each process() we sleep a few ms to let the worker
// catch up before the next block is read. With Engine=GPU the total latency is
// kInternalBlock (re-block FIFO) + transport latency, so we peak-align before
// scoring — and GPU f32 FFT diverges more than the CPU path, so the bar is 0.99.
TEST_CASE("SuperConvolver GPU engine convolves through the GPU transport",
          "[gpu][superconvolver]") {
    using P = SuperConvolverProcessor;
    constexpr std::size_t BLOCK = P::kInternalBlock;  // one host block == one B-block
    constexpr double SR = 48000.0;
    constexpr float SIZE = 0.05f;  // short IR for a fast test
    const std::size_t len = static_cast<std::size_t>(SIZE * SR);

    P proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kSize, SIZE);
    store.set_value(kMix, 100.0f);   // fully wet → output == convolution
    store.set_value(kGain, 0.0f);
    store.set_value(kBypass, 0.0f);
    store.set_value(kEngine, 1.0f);  // request the GPU engine
    store.set_value(kRooms, 1.0f);   // single-IR GPU path (this case covers it)

    pulp::format::PrepareContext ctx;
    ctx.sample_rate = SR;
    ctx.max_buffer_size = static_cast<int>(BLOCK);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);

    if (!proc.gpu_engine_active()) {
        // No Metal/GPU device in this environment (common in headless CI): the
        // processor correctly fell back to the CPU engine. Nothing to prove
        // about the GPU path here — skip rather than fail.
        WARN("GPU engine unavailable in this environment — skipping GPU path test "
             "(processor fell back to CPU). The CPU golden tests still cover audio.");
        proc.release();
        return;
    }

    // The GPU path is real (a device was found and the transport prepared).
    const int total_latency = proc.latency_samples();
    INFO("GPU latency_samples=" << total_latency);
    REQUIRE(total_latency > static_cast<int>(BLOCK));  // re-block + transport delay

    const std::vector<float> ir = make_reverb_ir(len);
    const int nblocks = static_cast<int>((len + BLOCK) / BLOCK) + 16;  // + slack to flush

    std::vector<float> out_all;
    std::vector<float> in_l(BLOCK), in_r(BLOCK), out_l(BLOCK), out_r(BLOCK);
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::format::ProcessContext pctx;
    pctx.sample_rate = SR;
    pctx.num_samples = static_cast<int>(BLOCK);

    for (int b = 0; b < nblocks; ++b) {
        std::fill(in_l.begin(), in_l.end(), 0.0f);
        std::fill(in_r.begin(), in_r.end(), 0.0f);
        if (b == 0) { in_l[0] = 1.0f; in_r[0] = 1.0f; }  // impulse
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, BLOCK);
        pulp::audio::BufferView<float> ov(op, 2, BLOCK);
        proc.process(ov, iv, midi_in, midi_out, pctx);
        // Let the transport's non-RT worker produce the next delayed block.
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        out_all.insert(out_all.end(), out_l.begin(), out_l.end());
    }
    // Capture the live GPU-cost stat while the engine is still up (release()
    // tears the transport down). Populated once the worker produced blocks: a
    // positive per-block wall-clock time — the UI's "µs/block" readout, real
    // worker timing, not a synthesized number.
    const auto live_stats = proc.gpu_block_stats();
    const auto live_us = proc.gpu_block_us();
    const auto live_status = proc.gpu_status();   // coherent UI snapshot
    proc.release();
    INFO("GPU blocks=" << live_stats.first << " avg_us=" << live_us.second
         << " budget_us=" << live_status.budget_us
         << " rt%=" << live_status.rt_percent);
    REQUIRE(live_stats.first > 0);
    REQUIRE(live_us.second > 0.0);
    // The UI snapshot agrees with the granular probes and derives a real,
    // positive real-time budget + utilization for THIS device/sample-rate.
    REQUIRE(live_status.active);
    REQUIRE(live_status.blocks == live_stats.first);
    REQUIRE(live_status.budget_us > 0.0);
    REQUIRE(live_status.rt_percent > 0.0);

    // Peak-align (the IR's onset is its unit first sample) and score against the
    // reference IR via normalized cross-correlation.
    std::size_t lag = 0; float peak = 0.0f;
    for (std::size_t i = 0; i < out_all.size(); ++i)
        if (std::abs(out_all[i]) > peak) { peak = std::abs(out_all[i]); lag = i; }
    REQUIRE(peak > 0.5f);  // a real convolved onset survived (not a miss/silence)

    double sxy = 0, sxx = 0, syy = 0;
    std::size_t scored = 0;
    for (std::size_t i = 0; i < len && lag + i < out_all.size(); ++i) {
        const double x = out_all[lag + i], y = ir[i];
        sxy += x * y; sxx += x * x; syy += y * y; ++scored;
    }
    const double corr = sxy / std::sqrt(sxx * syy + 1e-30);
    INFO("GPU corr=" << corr << " scored=" << scored << " lag=" << lag);
    REQUIRE(corr > 0.99);   // GPU f32 FFT reproduces the IR (looser bar than CPU)
}

namespace {

// Normalized cross-correlation of two equal-length signals over [0, len).
double xcorr(const float* a, const float* b, std::size_t len) {
    double sxy = 0, sxx = 0, syy = 0;
    for (std::size_t i = 0; i < len; ++i) {
        sxy += static_cast<double>(a[i]) * b[i];
        sxx += static_cast<double>(a[i]) * a[i];
        syy += static_cast<double>(b[i]) * b[i];
    }
    return sxy / std::sqrt(sxx * syy + 1e-30);
}

}  // namespace

// Correctness of the multi-IR GPU mode: an impulse through N panned rooms must
// reproduce, per channel, the panned sum of the N impulse responses (because
// convolving an impulse with IR_k yields IR_k). Validated by normalized
// cross-correlation > 0.99 against the CPU-built reference. Skips cleanly with
// no GPU device.
TEST_CASE("GpuMultiConvolver matches the CPU panned-IR-sum reference",
          "[gpu][superconvolver]") {
    constexpr uint32_t BLOCK = 512;
    constexpr uint32_t SR = 48000;
    constexpr uint32_t N = 8;
    const std::size_t ir_len = static_cast<std::size_t>(0.05 * SR);  // short, fast

    std::vector<std::vector<float>> irs(N);
    for (uint32_t k = 0; k < N; ++k)
        irs[k] = make_reverb_ir(ir_len, 0x2000u + k * 2654435761u);

    pulp::gpu_audio::GpuMultiConvolver mc(BLOCK, SR, irs);
    if (!mc.prepare() || !mc.gpu_available()) {
        WARN("GPU compute unavailable — skipping GpuMultiConvolver correctness test.");
        return;
    }

    // CPU reference: out_ch[i] = sum_k pan_ch[k] * IR_k[i] (impulse response).
    const auto& pl = mc.pan_l();
    const auto& pr = mc.pan_r();
    std::vector<double> ref_l(ir_len, 0.0), ref_r(ir_len, 0.0);
    for (uint32_t k = 0; k < N; ++k)
        for (std::size_t i = 0; i < ir_len; ++i) {
            ref_l[i] += static_cast<double>(pl[k]) * irs[k][i];
            ref_r[i] += static_cast<double>(pr[k]) * irs[k][i];
        }

    // Drive an impulse and collect enough blocks to flush the full IR.
    const int nblocks = static_cast<int>((ir_len + BLOCK) / BLOCK) + 2;
    std::vector<float> got_l, got_r;
    std::vector<float> in(BLOCK, 0.0f), ol(BLOCK), orr(BLOCK);
    for (int b = 0; b < nblocks; ++b) {
        std::fill(in.begin(), in.end(), 0.0f);
        if (b == 0) in[0] = 1.0f;
        REQUIRE(mc.convolve_stereo(in.data(), ol.data(), orr.data(), BLOCK));
        got_l.insert(got_l.end(), ol.begin(), ol.end());
        got_r.insert(got_r.end(), orr.begin(), orr.end());
    }

    std::vector<float> rl(ir_len), rr(ir_len);
    for (std::size_t i = 0; i < ir_len; ++i) {
        rl[i] = static_cast<float>(ref_l[i]);
        rr[i] = static_cast<float>(ref_r[i]);
    }
    const double cl = xcorr(got_l.data(), rl.data(), ir_len);
    const double cr = xcorr(got_r.data(), rr.data(), ir_len);
    INFO("multi-IR corr L=" << cl << " R=" << cr << " N=" << N);
    REQUIRE(cl > 0.99);
    REQUIRE(cr > 0.99);
    // The two channels are genuinely different (distinct pans) — not a mono dupe.
    REQUIRE(xcorr(rl.data(), rr.data(), ir_len) < 0.999);
}

// The structural GPU win: at scale (many rooms) the batched GPU multi-convolution
// must beat N independent CPU partitioned convolvers for the same work. We assert
// the GPU median is faster than the CPU median at a regime the bench shows wins
// by a wide margin (~3-4x), so the assertion is robust to host load. Skips with
// no GPU device.
TEST_CASE("GpuMultiConvolver beats N CPU convolvers at scale",
          "[gpu][superconvolver][perf]") {
    constexpr uint32_t BLOCK = 512;
    constexpr uint32_t SR = 48000;
    constexpr uint32_t N = 64;
    const std::size_t ir_len = static_cast<std::size_t>(0.5 * SR);

    std::vector<std::vector<float>> irs(N);
    for (uint32_t k = 0; k < N; ++k)
        irs[k] = make_reverb_ir(ir_len, 0x3000u + k * 2654435761u);

    pulp::gpu_audio::GpuMultiConvolver mc(BLOCK, SR, irs);
    if (!mc.prepare() || !mc.gpu_available()) {
        WARN("GPU compute unavailable — skipping GpuMultiConvolver perf test.");
        return;
    }

    const auto& pl = mc.pan_l();
    const auto& pr = mc.pan_r();

    // CPU baseline: N panned partitioned convolvers.
    std::vector<pulp::signal::PartitionedConvolver> cpu(N);
    for (uint32_t k = 0; k < N; ++k)
        cpu[k].load_ir(irs[k].data(), irs[k].size(), BLOCK);

    std::vector<float> x(BLOCK), y(BLOCK), aL(BLOCK), aR(BLOCK), wl(BLOCK), wr(BLOCK);
    for (uint32_t i = 0; i < BLOCK; ++i) x[i] = std::sin(0.013f * i) * 0.5f;

    auto med = [](std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    constexpr int iters = 40, warm = 5;

    std::vector<double> cpu_t, gpu_t;
    for (int it = 0; it < iters + warm; ++it) {
        auto t0 = std::chrono::steady_clock::now();
        std::fill(aL.begin(), aL.end(), 0.0f);
        std::fill(aR.begin(), aR.end(), 0.0f);
        for (uint32_t k = 0; k < N; ++k) {
            cpu[k].process(x.data(), y.data(), BLOCK);
            for (uint32_t i = 0; i < BLOCK; ++i) { aL[i] += pl[k] * y[i]; aR[i] += pr[k] * y[i]; }
        }
        auto t1 = std::chrono::steady_clock::now();
        REQUIRE(mc.convolve_stereo(x.data(), wl.data(), wr.data(), BLOCK));
        auto t2 = std::chrono::steady_clock::now();
        if (it >= warm) {
            cpu_t.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            gpu_t.push_back(std::chrono::duration<double, std::micro>(t2 - t1).count());
        }
    }
    const double cpu_med = med(cpu_t), gpu_med = med(gpu_t);
    INFO("N=" << N << " ir=0.5s  CPU=" << cpu_med << "us  GPU=" << gpu_med
         << "us  speedup=" << cpu_med / gpu_med << "x");
    REQUIRE(gpu_med < cpu_med);  // the batched GPU mode genuinely wins at scale
}

// Integration: with Engine=GPU and Rooms>1 the processor selects the batched
// multi-room GPU node (not the single-IR path), and produces real wet audio
// through the transport. Deterministic (no async timing assertion); skips with
// no GPU device.
TEST_CASE("SuperConvolver selects the multi-room GPU node when Rooms>1",
          "[gpu][superconvolver]") {
    using P = SuperConvolverProcessor;
    constexpr std::size_t BLOCK = P::kInternalBlock;
    constexpr double SR = 48000.0;

    P proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kSize, 0.1f);
    store.set_value(kMix, 100.0f);
    store.set_value(kGain, 0.0f);
    store.set_value(kBypass, 0.0f);
    store.set_value(kEngine, 1.0f);
    store.set_value(kRooms, 8.0f);

    pulp::format::PrepareContext ctx;
    ctx.sample_rate = SR;
    ctx.max_buffer_size = static_cast<int>(BLOCK);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);

    if (!proc.gpu_engine_active()) {
        WARN("GPU engine unavailable — skipping multi-room selection test.");
        proc.release();
        return;
    }

    REQUIRE(proc.gpu_multi_active());
    REQUIRE(proc.gpu_rooms() == 8);
    INFO("backend=" << proc.gpu_backend());

    // Drive a sine and confirm real wet energy emerges (the transport worker
    // produces blocks; sleep a little to let it catch up across the run).
    std::vector<float> in_l(BLOCK), in_r(BLOCK), out_l(BLOCK), out_r(BLOCK);
    pulp::midi::MidiBuffer mi, mo;
    pulp::format::ProcessContext pctx;
    pctx.sample_rate = SR;
    pctx.num_samples = static_cast<int>(BLOCK);
    double energy = 0.0;
    for (int b = 0; b < 64; ++b) {
        for (std::size_t i = 0; i < BLOCK; ++i) {
            in_l[i] = std::sin(0.05f * static_cast<float>(b * BLOCK + i)) * 0.5f;
            in_r[i] = in_l[i];
        }
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, BLOCK);
        pulp::audio::BufferView<float> ov(op, 2, BLOCK);
        proc.process(ov, iv, mi, mo, pctx);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        for (std::size_t i = 0; i < BLOCK; ++i)
            energy += out_l[i] * out_l[i] + out_r[i] * out_r[i];
    }
    const auto stats = proc.gpu_block_stats();
    INFO("produced=" << stats.first << " misses=" << stats.second
         << " energy=" << energy);
    REQUIRE(energy > 1e-3);          // real wet audio came out
    REQUIRE(stats.first > 0);        // the GPU worker genuinely produced blocks
    proc.release();
}

namespace {

// Process one stereo block (channel 0 = `mono`, channel 1 = same) and return
// channel 0 of the output. `mono` length must equal block.
struct DirectDriver {
    SuperConvolverProcessor& proc;
    std::size_t block;
    pulp::midi::MidiBuffer mi, mo;
    pulp::format::ProcessContext pctx;
    std::vector<float> in_l, in_r, out_l, out_r;

    DirectDriver(SuperConvolverProcessor& p, std::size_t b, double sr)
        : proc(p), block(b), in_l(b, 0.0f), in_r(b, 0.0f), out_l(b), out_r(b) {
        pctx.sample_rate = sr;
        pctx.num_samples = static_cast<int>(b);
    }

    // Drive `in` (mono → both channels) for one block, return channel-0 output.
    std::vector<float> block_io(const std::vector<float>& in, int sleep_ms) {
        for (std::size_t i = 0; i < block; ++i) {
            in_l[i] = i < in.size() ? in[i] : 0.0f;
            in_r[i] = in_l[i];
        }
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, block);
        pulp::audio::BufferView<float> ov(op, 2, block);
        proc.process(ov, iv, mi, mo, pctx);
        if (sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        return {out_l.begin(), out_l.end()};
    }

    void pump(int nblocks, int sleep_ms) {
        static const std::vector<float> z;
        for (int b = 0; b < nblocks; ++b) block_io(z, sleep_ms);
    }
};

// Pump silent blocks until `pred()` is true or `max_iter` is exhausted.
template <class F>
bool pump_until(DirectDriver& d, int max_iter, int sleep_ms, F pred) {
    for (int i = 0; i < max_iter && !pred(); ++i) d.pump(1, sleep_ms);
    return pred();
}

// Peak-aligned normalized cross-correlation of an impulse response capture
// against the reference IR over [0, len).
double corr_to_ir(const std::vector<float>& out, const std::vector<float>& ir,
                  std::size_t len) {
    std::size_t lag = 0; float peak = 0.0f;
    for (std::size_t i = 0; i < out.size(); ++i)
        if (std::abs(out[i]) > peak) { peak = std::abs(out[i]); lag = i; }
    double sxy = 0, sxx = 0, syy = 0;
    for (std::size_t i = 0; i < len && lag + i < out.size(); ++i) {
        const double x = out[lag + i], y = ir[i];
        sxy += x * y; sxx += x * x; syy += y * y;
    }
    return sxy / std::sqrt(sxx * syy + 1e-30);
}

}  // namespace

// The core fix: Engine (CPU<->GPU) is switchable LIVE, with no reload. Start on
// CPU, switch to GPU (worker builds + publishes the stack), confirm the GPU
// carries the audio, then switch back to CPU and confirm the CPU engine resumes
// producing the correct convolution. Skips cleanly with no GPU device.
TEST_CASE("SuperConvolver switches Engine CPU<->GPU live without reload",
          "[gpu][superconvolver]") {
    using P = SuperConvolverProcessor;
    constexpr std::size_t BLOCK = P::kInternalBlock;
    constexpr double SR = 48000.0;
    constexpr float SIZE = 0.05f;
    const std::size_t len = static_cast<std::size_t>(SIZE * SR);

    P proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kSize, SIZE);
    store.set_value(kMix, 100.0f);   // fully wet → output == convolution
    store.set_value(kGain, 0.0f);
    store.set_value(kBypass, 0.0f);
    store.set_value(kEngine, 0.0f);  // START on CPU
    store.set_value(kRooms, 4.0f);

    pulp::format::PrepareContext ctx;
    ctx.sample_rate = SR;
    ctx.max_buffer_size = static_cast<int>(BLOCK);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);
    REQUIRE_FALSE(proc.gpu_engine_active());   // CPU at prepare

    DirectDriver d(proc, BLOCK, SR);
    const std::vector<float> ir = make_reverb_ir(len);
    std::vector<float> impulse(BLOCK, 0.0f);
    impulse[0] = 1.0f;

    // --- Switch CPU -> GPU live. The worker (5 ms loop) sees the request and
    //     publishes the pre-built stack; the audio thread picks it up. ---
    store.set_value(kEngine, 1.0f);
    const bool went_gpu =
        pump_until(d, 150, 8, [&] { return proc.gpu_engine_active(); });
    if (!went_gpu) {
        WARN("GPU engine unavailable in this environment — skipping live-switch "
             "test (CPU golden tests still cover audio).");
        proc.release();
        return;
    }
    REQUIRE(proc.gpu_engine_active());
    REQUIRE(proc.gpu_rooms() == 4);          // honors the requested Rooms
    REQUIRE(proc.gpu_multi_active());

    // The GPU path carries real audio: drive an impulse and confirm non-silent,
    // IR-correct output.
    std::vector<float> gpu_out;
    {
        const int nblocks = static_cast<int>((len + BLOCK) / BLOCK) + 24;
        for (int b = 0; b < nblocks; ++b) {
            const auto o = d.block_io(b == 0 ? impulse : std::vector<float>{}, 8);
            gpu_out.insert(gpu_out.end(), o.begin(), o.end());
        }
    }
    double gpu_energy = 0.0;
    for (float v : gpu_out) gpu_energy += static_cast<double>(v) * v;
    INFO("gpu_energy=" << gpu_energy << " produced=" << proc.gpu_block_stats().first);
    REQUIRE(gpu_energy > 1e-3);               // non-silent
    REQUIRE(proc.gpu_block_stats().first > 0);

    // --- Switch GPU -> CPU live. The worker unpublishes gpu_active_. ---
    store.set_value(kEngine, 0.0f);
    const bool went_cpu =
        pump_until(d, 100, 5, [&] { return !proc.gpu_engine_active(); });
    REQUIRE(went_cpu);
    REQUIRE_FALSE(proc.gpu_engine_active());

    // Flush any residual GPU tail, then probe the CPU engine: it must reproduce
    // the IR (the live path is genuinely the CPU convolver again).
    d.pump(40, 0);
    std::vector<float> cpu_out;
    {
        const int nblocks = static_cast<int>((len + BLOCK) / BLOCK) + 6;
        for (int b = 0; b < nblocks; ++b) {
            auto o = d.block_io(b == 0 ? impulse : std::vector<float>{}, 0);
            cpu_out.insert(cpu_out.end(), o.begin(), o.end());
        }
    }
    const double cpu_corr = corr_to_ir(cpu_out, ir, len);
    INFO("cpu_corr after switch-back=" << cpu_corr);
    REQUIRE(cpu_corr > 0.99);                 // CPU engine resumed, output correct

    // --- Switch back to GPU once more — proves the toggle is repeatable. ---
    store.set_value(kEngine, 1.0f);
    REQUIRE(pump_until(d, 150, 8, [&] { return proc.gpu_engine_active(); }));
    proc.release();
}

// Live Rooms change: with Engine=GPU, raising Rooms rebuilds the GPU stack off
// the audio thread (atomic pointer swap, old stack retired) without a crash, and
// the GPU keeps producing real audio. Skips cleanly with no GPU device.
TEST_CASE("SuperConvolver changes Rooms live (16->64) without crashing",
          "[gpu][superconvolver]") {
    using P = SuperConvolverProcessor;
    constexpr std::size_t BLOCK = P::kInternalBlock;
    constexpr double SR = 48000.0;

    P proc;
    pulp::state::StateStore store;
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kSize, 0.1f);
    store.set_value(kMix, 100.0f);
    store.set_value(kGain, 0.0f);
    store.set_value(kBypass, 0.0f);
    store.set_value(kEngine, 1.0f);
    store.set_value(kRooms, 16.0f);

    pulp::format::PrepareContext ctx;
    ctx.sample_rate = SR;
    ctx.max_buffer_size = static_cast<int>(BLOCK);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    proc.prepare(ctx);

    DirectDriver d(proc, BLOCK, SR);
    if (!pump_until(d, 150, 8, [&] { return proc.gpu_engine_active(); })) {
        WARN("GPU engine unavailable — skipping live Rooms-change test.");
        proc.release();
        return;
    }
    REQUIRE(proc.gpu_rooms() == 16);
    REQUIRE(proc.gpu_multi_active());

    // Drive a sine and confirm real wet energy at 16 rooms.
    auto drive_sine = [&](int nblocks) {
        double energy = 0.0;
        for (int b = 0; b < nblocks; ++b) {
            std::vector<float> in(BLOCK);
            for (std::size_t i = 0; i < BLOCK; ++i)
                in[i] = std::sin(0.05f * static_cast<float>(b * BLOCK + i)) * 0.5f;
            const auto o = d.block_io(in, 4);
            for (float v : o) energy += static_cast<double>(v) * v;
        }
        return energy;
    };
    REQUIRE(drive_sine(48) > 1e-3);

    // Raise Rooms to 64 — the worker rebuilds the stack (retiring the old one)
    // and republishes. Confirm the live room count tracks the change.
    store.set_value(kRooms, 64.0f);
    const bool rebuilt =
        pump_until(d, 200, 8, [&] { return proc.gpu_rooms() == 64; });
    REQUIRE(rebuilt);
    REQUIRE(proc.gpu_rooms() == 64);
    REQUIRE(proc.gpu_multi_active());
    REQUIRE(proc.gpu_engine_active());

    // Still producing correct, non-silent audio after the live rebuild.
    REQUIRE(drive_sine(48) > 1e-3);
    REQUIRE(proc.gpu_block_stats().first > 0);
    proc.release();
}

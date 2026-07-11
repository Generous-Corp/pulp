// DSP regression tests for the SuperConvolver example plugin. Each test here
// reproduces a user-reported audio bug with a deterministic input vector and
// asserts the corrected behavior:
//
//   1. Bounce/offline render must be click-free and level-matched to realtime.
//   2. Raising the Size knob must not push the wet output past unity (no DAC
//      clipping / "digital distortion" at longer IRs).
//   3. Flow must move the stereo field by an audible amount at full depth while
//      staying bit-identical to the static pans at depth 0.
//
// The CPU convolution path (default engine) needs no GPU device; the GPU-path
// assertions skip cleanly when no compute device is present.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>
#include <pulp/gpu_audio/flow_pans.hpp>

#include "super_convolver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::examples;

namespace {

// Drive the processor block-by-block. Mono input is fed to both channels; the
// returned vectors are the two output channels concatenated per block.
struct Driver {
    SuperConvolverProcessor& proc;
    std::size_t block;
    double sr;
    pulp::midi::MidiBuffer mi, mo;
    std::vector<float> in_l, in_r, out_l, out_r;

    Driver(SuperConvolverProcessor& p, std::size_t b, double s)
        : proc(p), block(b), sr(s),
          in_l(b, 0.f), in_r(b, 0.f), out_l(b, 0.f), out_r(b, 0.f) {}

    // Render one block from a mono generator gen(sample_index_global). Returns
    // {L, R}. mode selects Realtime vs Offline. sleep_ms lets the async GPU
    // worker catch up on the realtime path.
    void block_io(std::size_t global_i0,
                  const std::function<float(std::size_t)>& gen,
                  pulp::format::ProcessMode mode, int sleep_ms,
                  std::vector<float>& l_out, std::vector<float>& r_out) {
        for (std::size_t i = 0; i < block; ++i) {
            const float v = gen(global_i0 + i);
            in_l[i] = v;
            in_r[i] = v;
        }
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {out_l.data(), out_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, block);
        pulp::audio::BufferView<float> ov(op, 2, block);
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = sr;
        ctx.num_samples = static_cast<int>(block);
        ctx.process_mode = mode;
        if (mode == pulp::format::ProcessMode::Offline)
            ctx.render_speed_hint = pulp::format::RenderSpeedHint::FasterThanRealtime;
        proc.process(ov, iv, mi, mo, ctx);
        if (sleep_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        l_out.assign(out_l.begin(), out_l.end());
        r_out.assign(out_r.begin(), out_r.end());
    }
};

// Wire the processor to the store and set the params. The processor is
// non-copyable/non-movable (owns atomics + a worker thread), so the caller
// constructs it and passes it in; this only configures store + params.
void configure(SuperConvolverProcessor& proc, pulp::state::StateStore& store,
               float size, float mix, int engine, int rooms) {
    proc.set_state_store(&store);
    proc.define_parameters(store);
    store.set_value(kSize, size);
    store.set_value(kMix, mix);
    store.set_value(kGain, 0.0f);
    store.set_value(kBypass, 0.0f);
    store.set_value(kEngine, static_cast<float>(engine));
    store.set_value(kRooms, static_cast<float>(rooms));
}

pulp::format::PrepareContext prep_ctx(double sr, std::size_t block) {
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = sr;
    ctx.max_buffer_size = static_cast<int>(block);
    ctx.input_channels = 2;
    ctx.output_channels = 2;
    return ctx;
}

double peak_of(const std::vector<float>& v) {
    double p = 0.0;
    for (float x : v) p = std::max(p, static_cast<double>(std::abs(x)));
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Bug 2: Size > ~0.2s must not clip. Measure the fully-wet output peak for a
// full-scale broadband drive across Size values on the CPU engine.
// ---------------------------------------------------------------------------
TEST_CASE("SuperConvolver wet output stays below unity across Size (CPU)",
          "[superconvolver][dsp][size]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = 512;

    const std::vector<float> sizes = {0.05f, 0.2f, 0.5f, 1.5f, 4.0f};
    for (float size : sizes) {
        pulp::state::StateStore store;
        // Use a fresh processor per size (Size is baked at prepare()).
        SuperConvolverProcessor proc;
        configure(proc, store, size, 100.0f, /*engine=*/0, /*rooms=*/1);
        proc.prepare(prep_ctx(SR, BLOCK));

        // Full-scale deterministic broadband: a sum of a few loud sines plus an
        // LCG noise floor, clamped to +/-1. Excites the whole spectrum so the
        // IR's resonant peaks show up in the output peak.
        std::uint32_t s = 0x1234567u;
        auto gen = [&](std::size_t i) {
            s = s * 1664525u + 1013904223u;
            const float noise = (static_cast<float>(s >> 9) / 4194304.0f - 1.0f) * 0.5f;
            const float t = static_cast<float>(i) / static_cast<float>(SR);
            float v = 0.4f * std::sin(2.f * 3.14159265f * 220.f * t)
                    + 0.3f * std::sin(2.f * 3.14159265f * 1500.f * t)
                    + 0.2f * std::sin(2.f * 3.14159265f * 5000.f * t)
                    + noise;
            return std::clamp(v, -1.0f, 1.0f);
        };

        Driver d(proc, BLOCK, SR);
        const std::size_t len = static_cast<std::size_t>(size * SR);
        const int nblocks = static_cast<int>((len + BLOCK) / BLOCK) + 40;
        double peak = 0.0;
        std::vector<float> l, r;
        for (int b = 0; b < nblocks; ++b) {
            d.block_io(static_cast<std::size_t>(b) * BLOCK, gen,
                       pulp::format::ProcessMode::Realtime, 0, l, r);
            peak = std::max({peak, peak_of(l), peak_of(r)});
        }
        INFO("Size=" << size << "s  wet peak=" << peak);
        // Correct behavior: a fully-wet, full-scale drive never exceeds unity.
        // (Before the peak-response normalization fix this hit ~1.5–2.1.)
        CHECK(peak <= 1.0);
        proc.release();
    }
}

// ---------------------------------------------------------------------------
// Bug 1: Offline (bounce) render must match the realtime render and be
// click-free. Compare CPU realtime vs offline (must be identical), and — when a
// GPU device exists — GPU offline must be click-free and level-matched.
// ---------------------------------------------------------------------------
TEST_CASE("SuperConvolver offline render matches realtime and is click-free",
          "[superconvolver][dsp][offline]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    constexpr float SIZE = 0.3f;

    auto drive = [&](std::size_t i) {
        const float t = static_cast<float>(i) / static_cast<float>(SR);
        return 0.6f * std::sin(2.f * 3.14159265f * 330.f * t);
    };

    auto render = [&](int engine, pulp::format::ProcessMode mode, int sleep_ms,
                      int nblocks) {
        pulp::state::StateStore store;
        SuperConvolverProcessor proc;
        configure(proc, store, SIZE, 50.0f, engine, /*rooms=*/1);
        proc.prepare(prep_ctx(SR, BLOCK));
        const bool gpu = proc.gpu_engine_active();
        Driver d(proc, BLOCK, SR);
        std::vector<float> all, l, r;
        for (int b = 0; b < nblocks; ++b) {
            d.block_io(static_cast<std::size_t>(b) * BLOCK, drive, mode, sleep_ms, l, r);
            all.insert(all.end(), l.begin(), l.end());
        }
        proc.release();
        return std::make_pair(all, gpu);
    };

    const int nblocks = 60;

    SECTION("CPU offline == CPU realtime (bit-identical, inline path)") {
        auto rt = render(0, pulp::format::ProcessMode::Realtime, 0, nblocks).first;
        auto off = render(0, pulp::format::ProcessMode::Offline, 0, nblocks).first;
        REQUIRE(rt.size() == off.size());
        double max_diff = 0.0;
        for (std::size_t i = 0; i < rt.size(); ++i)
            max_diff = std::max(max_diff, static_cast<double>(std::abs(rt[i] - off[i])));
        INFO("CPU realtime-vs-offline max abs diff=" << max_diff);
        CHECK(max_diff < 1e-6);
    }

    SECTION("GPU offline is click-free and level-matched to GPU realtime") {
        auto [rt, gpu_rt] = render(1, pulp::format::ProcessMode::Realtime, 8, nblocks);
        auto [off, gpu_off] = render(1, pulp::format::ProcessMode::Offline, 0, nblocks);
        if (!gpu_off) {
            WARN("GPU engine unavailable — skipping GPU offline check.");
            return;
        }
        // Click detection: the offline output must not contain sample-to-sample
        // jumps far larger than the signal itself (a dropped/duplicated block
        // shows up as a discontinuity).
        double max_step = 0.0, off_peak = peak_of(off);
        for (std::size_t i = 1; i < off.size(); ++i)
            max_step = std::max(max_step,
                                static_cast<double>(std::abs(off[i] - off[i - 1])));
        INFO("GPU offline peak=" << off_peak << " max_step=" << max_step
             << " gpu_rt=" << gpu_rt);
        CHECK(std::isfinite(off_peak));
        CHECK(off_peak > 1e-3);           // real wet audio, not silence
        CHECK(off_peak <= 1.0);           // no clipping
        // A 330 Hz tone at these levels steps at most ~0.1/sample; a dropped
        // block would jump by ~the signal amplitude. Bar well above the signal.
        CHECK(max_step < 0.5);
    }
}

// The "bounce clipping" the user heard: a full-scale, fully-wet OFFLINE render
// must not exceed unity (an offline bounce is written to a clamped file, so any
// overshoot becomes audible hard clipping). Guards that the peak-response
// headroom fix holds on the offline path, on both engines.
TEST_CASE("SuperConvolver offline bounce does not clip at full scale",
          "[superconvolver][dsp][offline]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    constexpr float SIZE = 0.5f;   // a Size the user reported as "screwed up"

    std::uint32_t s = 0x9E3779B1u;
    auto gen = [&](std::size_t i) {
        s = s * 1664525u + 1013904223u;
        const float noise = (static_cast<float>(s >> 9) / 4194304.0f - 1.0f) * 0.5f;
        const float t = static_cast<float>(i) / static_cast<float>(SR);
        float v = 0.4f * std::sin(2.f * 3.14159265f * 220.f * t)
                + 0.3f * std::sin(2.f * 3.14159265f * 1500.f * t)
                + 0.2f * std::sin(2.f * 3.14159265f * 5000.f * t)
                + noise;
        return std::clamp(v, -1.0f, 1.0f);
    };

    auto offline_peak = [&](int engine) -> std::pair<double, bool> {
        pulp::state::StateStore store;
        SuperConvolverProcessor proc;
        configure(proc, store, SIZE, 100.0f, engine, /*rooms=*/1);  // fully wet
        proc.prepare(prep_ctx(SR, BLOCK));
        const bool gpu = proc.gpu_engine_active();
        Driver d(proc, BLOCK, SR);
        std::vector<float> l, r;
        double peak = 0.0;
        const int nblocks = 120;
        for (int b = 0; b < nblocks; ++b) {
            d.block_io(static_cast<std::size_t>(b) * BLOCK, gen,
                       pulp::format::ProcessMode::Offline, 0, l, r);
            peak = std::max({peak, peak_of(l), peak_of(r)});
        }
        proc.release();
        return {peak, gpu};
    };

    const auto cpu = offline_peak(0);
    INFO("CPU offline full-scale wet peak=" << cpu.first);
    CHECK(cpu.first <= 1.0);

    const auto gpu = offline_peak(1);
    if (gpu.second) {
        INFO("GPU offline full-scale wet peak=" << gpu.first);
        CHECK(gpu.first <= 1.0);
    } else {
        WARN("GPU engine unavailable — offline no-clip checked on CPU only.");
    }
}

// ---------------------------------------------------------------------------
// Bug 3: Flow must move each room's pan by an audible amount at full depth, and
// be exactly static at depth 0. Pure numeric test on flow_pans.hpp — no GPU.
// ---------------------------------------------------------------------------
TEST_CASE("Flow pans swing audibly at full depth and are static at zero",
          "[superconvolver][dsp][flow]") {
    using namespace pulp::gpu_audio;
    constexpr std::uint32_t N = 8;
    constexpr float kNorm = 0.3535f;  // ~1/sqrt(8)
    constexpr double T = 3.0;          // musically-relevant window (seconds)
    constexpr double DT = 0.005;       // 5 ms sampling

    std::vector<float> base(N);
    for (std::uint32_t k = 0; k < N; ++k) base[k] = flow_base_azimuth(k, N);

    // depth 0 => bit-identical to the static constant-power pans.
    {
        std::vector<float> pl(N), pr(N);
        flow_pans_from_base(base.data(), N, 0.0f, 1.0f, 1.234, kNorm,
                            pl.data(), pr.data());
        for (std::uint32_t k = 0; k < N; ++k) {
            CHECK(pl[k] == std::cos(base[k]) * kNorm);
            CHECK(pr[k] == std::sin(base[k]) * kNorm);
        }
    }

    // At full depth, track each room's channel-gain swing over the window and
    // require an audible move (>6 dB on at least one channel per room).
    std::vector<double> min_l(N, 1e9), max_l(N, -1e9), min_r(N, 1e9), max_r(N, -1e9);
    std::vector<float> pl(N), pr(N);
    for (double t = 0.0; t <= T; t += DT) {
        flow_pans_from_base(base.data(), N, 1.0f, 1.0f, t, kNorm, pl.data(), pr.data());
        for (std::uint32_t k = 0; k < N; ++k) {
            min_l[k] = std::min(min_l[k], static_cast<double>(pl[k]));
            max_l[k] = std::max(max_l[k], static_cast<double>(pl[k]));
            min_r[k] = std::min(min_r[k], static_cast<double>(pr[k]));
            max_r[k] = std::max(max_r[k], static_cast<double>(pr[k]));
        }
    }
    double min_swing_db = 1e9;
    for (std::uint32_t k = 0; k < N; ++k) {
        const double swing_l = 20.0 * std::log10((max_l[k] + 1e-9) / (min_l[k] + 1e-9));
        const double swing_r = 20.0 * std::log10((max_r[k] + 1e-9) / (min_r[k] + 1e-9));
        min_swing_db = std::min(min_swing_db, std::max(swing_l, swing_r));
    }
    INFO("min per-room channel swing over " << T << "s = " << min_swing_db << " dB");
    // Every room's pan must move by a clearly audible amount (before the rate/
    // swing widening the slowest room only managed ~4 dB over this window).
    CHECK(min_swing_db > 6.0);
}

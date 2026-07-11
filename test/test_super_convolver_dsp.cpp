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
#include <pulp/audio/audio_file.hpp>
#include <pulp/gpu_audio/flow_pans.hpp>

#include "super_convolver.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
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

// RMS of a channel over [start, end).
double rms_from(const std::vector<float>& v, std::size_t start) {
    if (v.size() <= start) return 0.0;
    double acc = 0.0;
    std::size_t n = 0;
    for (std::size_t i = start; i < v.size(); ++i) {
        acc += static_cast<double>(v[i]) * v[i];
        ++n;
    }
    return n ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

// Drive `proc` for `nblocks` blocks with separate L/R generators (decorrelated
// or identical) and return the concatenated {L, R} output. Offline mode captures
// the GPU path synchronously so the measurement is deterministic.
struct StereoResult { std::vector<float> l, r; bool gpu = false; };
StereoResult drive_stereo(SuperConvolverProcessor& proc, std::size_t block, double sr,
                          int nblocks,
                          const std::function<float(std::size_t)>& gen_l,
                          const std::function<float(std::size_t)>& gen_r,
                          pulp::format::ProcessMode mode) {
    StereoResult out;
    out.gpu = proc.gpu_engine_active();
    std::vector<float> in_l(block), in_r(block), o_l(block), o_r(block);
    pulp::midi::MidiBuffer mi, mo;
    for (int b = 0; b < nblocks; ++b) {
        const std::size_t g0 = static_cast<std::size_t>(b) * block;
        for (std::size_t i = 0; i < block; ++i) {
            in_l[i] = gen_l(g0 + i);
            in_r[i] = gen_r(g0 + i);
        }
        const float* ip[2] = {in_l.data(), in_r.data()};
        float* op[2] = {o_l.data(), o_r.data()};
        pulp::audio::BufferView<const float> iv(ip, 2, block);
        pulp::audio::BufferView<float> ov(op, 2, block);
        pulp::format::ProcessContext ctx;
        ctx.sample_rate = sr;
        ctx.num_samples = static_cast<int>(block);
        ctx.process_mode = mode;
        if (mode == pulp::format::ProcessMode::Offline)
            ctx.render_speed_hint = pulp::format::RenderSpeedHint::FasterThanRealtime;
        proc.process(ov, iv, mi, mo, ctx);
        out.l.insert(out.l.end(), o_l.begin(), o_l.end());
        out.r.insert(out.r.end(), o_r.begin(), o_r.end());
    }
    return out;
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

// ---------------------------------------------------------------------------
// Issue 1: toggling Engine CPU<->GPU must not jump the level. The multi-room
// GPU path used a 1/sqrt(N) constant-power pan whose decorrelated aggregate sat
// ~3 dB below the single-IR / CPU path — the "CPU is so much louder" the user
// heard at the default 16 rooms. With the sqrt(2/N) pan the decorrelated room
// aggregate is unity per channel, so a mono/correlated drive (the case a mono-
// send reverb can level-match) lands within ~1 dB of the CPU engine.
//
// Measured on this Mac's Metal device (SIZE=0.5, 48 kHz, broadband mono drive)
// before -> after the fix:
//   GPU single vs CPU : 0.00 dB      -> 0.00 dB   (per-channel path, always matched)
//   GPU multi  vs CPU : -2.60 dB     -> +0.41 dB  (pan normalization fixed)
// A genuinely decorrelated stereo drive stays ~3 dB below the CPU per-channel
// engine — the standard, expected behavior of a mono reverb send (0.5*(L+R)
// folds decorrelated content by 3 dB); the pan gain intentionally does NOT try
// to mask that.
// ---------------------------------------------------------------------------
TEST_CASE("SuperConvolver engines are level-matched (Issue 1)",
          "[superconvolver][dsp][level]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    constexpr float SIZE = 0.5f;
    const int nblocks = 120;
    const std::size_t warmup = 8 * BLOCK;  // skip latency priming + IR ramp

    // Representative correlated reverb input: the same broadband (whole-spectrum)
    // signal on L and R, i.e. a mono send. A mono-driven multi-room reverb can
    // level-match the CPU per-channel engine on this; a tonal drive over-coheres
    // the phase-scrambled rooms, so broadband is the fair comparison.
    auto broadband = [](std::size_t i) {
        std::uint32_t s = 0x5555u + static_cast<std::uint32_t>(i) * 2654435761u;
        s = s * 1664525u + 1013904223u;
        return (static_cast<float>(s >> 9) / 4194304.0f - 1.0f) * 0.25f;
    };

    auto wet_rms = [&](int engine, int rooms) {
        pulp::state::StateStore store;
        SuperConvolverProcessor proc;
        configure(proc, store, SIZE, 100.0f, engine, rooms);  // fully wet
        proc.prepare(prep_ctx(SR, BLOCK));
        const bool gpu = proc.gpu_engine_active();
        const int arooms = proc.gpu_rooms();
        auto r = drive_stereo(proc, BLOCK, SR, nblocks, broadband, broadband,
                              pulp::format::ProcessMode::Offline);
        proc.release();
        const double rms = 0.5 * (rms_from(r.l, warmup) + rms_from(r.r, warmup));
        return std::make_tuple(rms, gpu, arooms);
    };
    auto db = [](double a, double b) { return 20.0 * std::log10((a + 1e-12) / (b + 1e-12)); };

    const auto [cpu, cpu_gpu, cpu_rooms] = wet_rms(0, 1);
    REQUIRE(cpu > 1e-4);  // the CPU reference produced real wet audio

    // GPU single-IR path must match the CPU engine (same per-channel convolution).
    const auto [gs, gs_gpu, gs_rooms] = wet_rms(1, 1);
    if (gs_gpu) {
        INFO("GPU single vs CPU = " << db(gs, cpu) << " dB");
        CHECK(std::abs(db(gs, cpu)) <= 1.0);
    } else {
        WARN("GPU engine unavailable — single-IR level check skipped.");
    }

    // GPU multi-room path must be within ~1 dB of the CPU engine on a mono drive.
    const auto [gm, gm_gpu, gm_rooms] = wet_rms(1, 16);
    if (gm_gpu && gm_rooms > 1) {
        INFO("GPU multi (" << gm_rooms << " rooms) vs CPU = " << db(gm, cpu) << " dB");
        CHECK(std::abs(db(gm, cpu)) <= 1.0);
    } else {
        WARN("GPU multi-room unavailable — multi level check skipped.");
    }
}

// Full-scale, fully-wet must stay <= unity on EVERY engine path (no clipping the
// DAC / bounce file). The sqrt(2/N) pan is +3 dB vs the old 1/sqrt(N), so this
// guards that the IRs' 0 dB peak-response normalization still keeps the panned
// multi-room sum bounded. The CPU/single Size-sweep no-clip is covered above;
// this adds the multi-room path the pan change touches.
TEST_CASE("SuperConvolver full-scale fully-wet stays below unity on all paths",
          "[superconvolver][dsp][level][size]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    const int nblocks = 120;

    std::uint32_t s = 0x9E3779B1u;
    auto full_scale = [&](std::size_t i) {
        s = s * 1664525u + 1013904223u;
        const float noise = (static_cast<float>(s >> 9) / 4194304.0f - 1.0f) * 0.5f;
        const float t = static_cast<float>(i) / static_cast<float>(SR);
        float v = 0.4f * std::sin(2.f * 3.14159265f * 220.f * t)
                + 0.3f * std::sin(2.f * 3.14159265f * 1500.f * t)
                + 0.2f * std::sin(2.f * 3.14159265f * 5000.f * t) + noise;
        return std::clamp(v, -1.0f, 1.0f);
    };

    auto peak = [&](int engine, int rooms, float size) {
        pulp::state::StateStore store;
        SuperConvolverProcessor proc;
        configure(proc, store, size, 100.0f, engine, rooms);  // fully wet
        proc.prepare(prep_ctx(SR, BLOCK));
        const bool gpu = proc.gpu_engine_active();
        const int arooms = proc.gpu_rooms();
        auto r = drive_stereo(proc, BLOCK, SR, nblocks, full_scale, full_scale,
                              pulp::format::ProcessMode::Offline);
        proc.release();
        return std::make_tuple(std::max(peak_of(r.l), peak_of(r.r)), gpu, arooms);
    };

    // CPU single always available.
    const auto [cpu_pk, cpu_gpu, cpu_rooms] = peak(0, 1, 0.5f);
    INFO("CPU single full-scale peak=" << cpu_pk);
    CHECK(cpu_pk <= 1.0);

    // GPU multi-room across a couple of Sizes / room counts the pan change scales.
    for (auto cfg : {std::pair<int, float>{16, 0.5f}, std::pair<int, float>{32, 1.5f}}) {
        const auto [pk, gpu, rooms] = peak(1, cfg.first, cfg.second);
        if (gpu && rooms > 1) {
            INFO("GPU multi rooms=" << rooms << " size=" << cfg.second
                 << " full-scale peak=" << pk);
            CHECK(pk <= 1.0);
        } else {
            WARN("GPU multi-room unavailable — full-scale no-clip skipped for a cfg.");
        }
    }
}

// ---------------------------------------------------------------------------
// Issue 2: the Size knob was inert once a file IR was loaded. Two coverage
// points: (a) the windowing primitive that Size now applies to a loaded file,
// and (b) the end-to-end guarantee that moving Size with a file loaded actually
// rebuilds a shorter IR off-thread.
// ---------------------------------------------------------------------------

// (a) window_ir_to_length: shortening Size truncates a loaded IR's tail with a
// smooth fade (no click), never zero-pad-extends a shorter IR, and re-normalizes.
TEST_CASE("window_ir_to_length shortens a loaded IR with a fade tail (Issue 2)",
          "[superconvolver][dsp][size]") {
    // A long, constant-magnitude tail makes the truncation + fade easy to read.
    const std::size_t N = 20000;
    std::vector<float> ir(N, 0.5f);
    ir[0] = 1.0f;

    const std::size_t target = 4000;
    const std::size_t fade = 1000;
    const auto w = pulp::examples::window_ir_to_length(ir, target, fade);

    // Effective length shrinks to Size.
    REQUIRE(w.size() == target);

    const double body_rms = rms_from(std::vector<float>(w.begin(),
                                     w.begin() + (target - fade)), 0);
    // Deep tail (the last quarter of the fade, nearest the cut) is strongly
    // attenuated toward zero — the shorter Size audibly shortens the reverb...
    const double deep_tail_rms = rms_from(std::vector<float>(
        w.begin() + (target - fade / 4), w.end()), 0);
    CHECK(deep_tail_rms < 0.3 * body_rms);
    // ...and the last sample is driven essentially to zero (a click-free cut).
    CHECK(std::abs(w.back()) < 0.02 * peak_of(w));

    // A Size longer than the IR is a no-op: never zero-pad-extended.
    const auto same = pulp::examples::window_ir_to_length(ir, N + 5000, 1000);
    REQUIRE(same.size() == N);
    CHECK(same == ir);
}

// (b) End-to-end: with a real file IR loaded, moving Size must re-trigger an
// off-thread rebuild that shortens the effective IR (generation + length change).
TEST_CASE("Size knob shortens a loaded file IR (Issue 2)",
          "[superconvolver][dsp][size]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;

    // Write a 1.0 s decaying-noise IR to a temp WAV (float32 to keep the tail).
    const std::size_t file_len = static_cast<std::size_t>(1.0 * SR);
    pulp::audio::AudioFileData data;
    data.sample_rate = static_cast<uint32_t>(SR);
    data.channels.assign(1, std::vector<float>(file_len, 0.0f));
    std::uint32_t s = 0xBEEF1234u;
    for (std::size_t i = 0; i < file_len; ++i) {
        s = s * 1664525u + 1013904223u;
        const float white = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
        data.channels[0][i] = white * std::exp(-3.0f * static_cast<float>(i) / file_len);
    }
    data.channels[0][0] = 1.0f;
    const auto path = (std::filesystem::temp_directory_path() /
                       "pulp_superconvolver_size_ir.wav").string();
    REQUIRE(pulp::audio::write_wav_file(path, data, pulp::audio::WavBitDepth::Float32));

    pulp::state::StateStore store;
    SuperConvolverProcessor proc;
    // Size 2.0 s is LONGER than the 1.0 s file, so a loaded file passes through
    // at its own length (~1.0 s) — distinct from the 2.0 s synthetic fallback
    // (~2.0 s), which proves the file (not the synthetic IR) is the base.
    configure(proc, store, /*size=*/2.0f, /*mix=*/100.0f, /*engine=*/0, /*rooms=*/1);
    proc.set_ir_path(path);
    proc.prepare(prep_ctx(SR, BLOCK));

    REQUIRE(proc.ir_path() == path);
    const std::uint32_t gen0 = proc.ir_generation();
    const std::size_t len0 = proc.impulse_response_snapshot().size();
    // File loaded and passed through at ~1.0 s, not the 2.0 s synthetic length.
    INFO("len0=" << len0);
    CHECK(len0 > file_len / 2);
    CHECK(len0 < static_cast<std::size_t>(1.5 * SR));

    // Drive a few blocks so process() publishes the requested Size, then let the
    // background worker re-window off-thread.
    Driver d(proc, BLOCK, SR);
    std::vector<float> l, r;
    auto pump = [&](int blocks) {
        for (int b = 0; b < blocks; ++b)
            d.block_io(static_cast<std::size_t>(b) * BLOCK,
                       [](std::size_t) { return 0.0f; },
                       pulp::format::ProcessMode::Realtime, 5, l, r);
    };

    // Shorten Size to 0.2 s: the loaded 1.0 s IR must be truncated to ~0.2 s.
    store.set_value(kSize, 0.2f);
    pump(4);
    std::size_t len1 = proc.impulse_response_snapshot().size();
    std::uint32_t gen1 = proc.ir_generation();
    for (int tries = 0; tries < 200 && (gen1 == gen0 || len1 >= len0); ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        pump(1);
        len1 = proc.impulse_response_snapshot().size();
        gen1 = proc.ir_generation();
    }
    INFO("gen0=" << gen0 << " gen1=" << gen1 << " len0=" << len0 << " len1=" << len1);
    // The Size change re-triggered a rebuild (generation moved)...
    CHECK(gen1 != gen0);
    // ...to a materially shorter effective IR (~0.2 s vs ~1.0 s).
    CHECK(len1 < len0);
    CHECK(len1 < static_cast<std::size_t>(0.5 * SR));

    proc.release();
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

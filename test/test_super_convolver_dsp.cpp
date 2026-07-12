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
// It also covers the IR-source surface the web lanes depend on — the built-in IR
// family, the decoded-PCM entry point (the only IR source a host with no codec
// has), the SCv2 tagged state blob (plus its SCv1 / raw-path back-compat), and
// the thread-less rebuild contract: with the background worker disabled, only
// service_ir_rebuild() on the control thread may rebuild, never process().
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

// A deterministic PCM "impulse response": a sparse set of decaying reflections.
// Distinctive enough that the wet output of an impulse can be matched against it
// sample-for-sample.
std::vector<float> make_pcm_ir(std::size_t frames, std::uint32_t seed = 0xC0FFEEu) {
    std::vector<float> pcm(frames, 0.0f);
    if (frames == 0) return pcm;
    pcm[0] = 1.0f;
    std::uint32_t s = seed;
    for (std::size_t i = 1; i < frames; i += 37) {
        s = s * 1664525u + 1013904223u;
        const float white = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
        pcm[i] = white * std::exp(-4.0f * static_cast<float>(i) /
                                  static_cast<float>(frames));
    }
    return pcm;
}

// Normalized correlation of two equal-length buffers: 1 when b is a positive
// scaling of a (which is what a normalize-only pipeline produces).
double correlation(const std::vector<float>& a, const std::vector<float>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    double num = 0.0, da = 0.0, db = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        num += static_cast<double>(a[i]) * b[i];
        da += static_cast<double>(a[i]) * a[i];
        db += static_cast<double>(b[i]) * b[i];
    }
    if (da <= 0.0 || db <= 0.0) return 0.0;
    return num / std::sqrt(da * db);
}

// Render `nblocks` blocks of a mono generator through the CPU engine and return
// the left channel.
std::vector<float> render_mono(SuperConvolverProcessor& proc, std::size_t block,
                               double sr, int nblocks,
                               const std::function<float(std::size_t)>& gen) {
    Driver d(proc, block, sr);
    std::vector<float> all, l, r;
    for (int b = 0; b < nblocks; ++b) {
        d.block_io(static_cast<std::size_t>(b) * block, gen,
                   pulp::format::ProcessMode::Realtime, 0, l, r);
        all.insert(all.end(), l.begin(), l.end());
    }
    return all;
}

// The plugin's wet impulse response as the audio path actually renders it: drive
// a unit impulse and read back from the reported latency.
std::vector<float> measured_ir(SuperConvolverProcessor& proc, std::size_t block,
                               double sr, std::size_t want) {
    const std::size_t lat = static_cast<std::size_t>(proc.latency_samples());
    const int nblocks = static_cast<int>((lat + want) / block) + 2;
    const auto out = render_mono(proc, block, sr, nblocks,
                                 [](std::size_t i) { return i == 0 ? 1.0f : 0.0f; });
    REQUIRE(out.size() >= lat + want);
    return std::vector<float>(out.begin() + static_cast<std::ptrdiff_t>(lat),
                              out.begin() + static_cast<std::ptrdiff_t>(lat + want));
}

double max_abs_diff(const std::vector<float>& a, const std::vector<float>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    double d = 0.0;
    for (std::size_t i = 0; i < n; ++i)
        d = std::max(d, static_cast<double>(std::abs(a[i] - b[i])));
    return d;
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

// ---------------------------------------------------------------------------
// IR sources the web lanes depend on. A wasm build has no codec and no file
// dialog: the browser decodes the IR and hands the raw floats to set_ir_pcm(),
// and the built-in IR family is what the plugin plays with no I/O at all.
// ---------------------------------------------------------------------------

// set_ir_pcm() must actually reach the convolver: the wet impulse response the
// audio path renders is the supplied PCM (normalized), not the synthetic IR.
TEST_CASE("SuperConvolver set_ir_pcm becomes the live IR",
          "[superconvolver][dsp][ir-source]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    const std::size_t frames = 4800;  // 0.1 s — well under the 2.0 s Size window
    const auto pcm = make_pcm_ir(frames);

    pulp::state::StateStore store;
    SuperConvolverProcessor proc;
    configure(proc, store, /*size=*/2.0f, /*mix=*/100.0f, /*engine=*/0, /*rooms=*/1);
    proc.set_ir_pcm(pcm.data(), pcm.size(), 1, SR, "unit-test");
    proc.prepare(prep_ctx(SR, BLOCK));

    // Same rate, and Size is longer than the PCM, so the pipeline only sums to
    // mono and normalizes — the base IR is a positive scaling of the input.
    const auto base = proc.impulse_response_snapshot();
    REQUIRE(base.size() == frames);
    INFO("corr(pcm, base IR) = " << correlation(pcm, base));
    CHECK(correlation(pcm, base) > 0.999);
    CHECK(peak_of(base) > 1e-4);

    // ...and the audio path convolves with exactly that IR.
    const auto rendered = measured_ir(proc, BLOCK, SR, frames);
    INFO("max |rendered - base| = " << max_abs_diff(rendered, base));
    CHECK(max_abs_diff(rendered, base) < 1e-4);
    proc.release();
}

// set_ir_pcm() must BOUND what it retains. A browser user can drop a 3-minute
// WAV in (decodeAudioData succeeds, and the loader's reject_longer_than_seconds
// is 300 s), but the IR actually used is capped at kMaxIrSeconds downstream —
// so keeping the whole thing only inflates every serialize_plugin_state() blob
// (~34 MB for 3 minutes of stereo), which a DAW then writes into the project for
// each instance and a WAM host postMessages on every getState(). Mono-summing and
// truncating at the source is what makes the state format's own "bounded by
// kMaxIrSeconds" claim true.
TEST_CASE("SuperConvolver set_ir_pcm bounds the PCM it retains",
          "[superconvolver][dsp][ir-source]") {
    constexpr double SR = 48000.0;
    const std::size_t cap =
        static_cast<std::size_t>(SuperConvolverProcessor::kMaxIrSeconds * SR);

    SECTION("an over-long source is truncated to kMaxIrSeconds") {
        const std::size_t frames = cap * 2 + 12345;   // ~20 s
        const auto pcm = make_pcm_ir(frames);

        SuperConvolverProcessor proc;
        proc.set_ir_pcm(pcm.data(), frames, 1, SR, "over-long");
        CHECK(proc.ir_pcm_frames() == cap);

        // ...and the state blob is bounded with it (header + tag + 2 u32 + f32s).
        const auto blob = proc.serialize_plugin_state();
        CHECK(blob.size() <= SuperConvolverProcessor::kStateHeaderSize + 1 + 8 +
                                 cap * sizeof(float));
    }

    SECTION("a source at or under the cap is kept whole") {
        const std::size_t frames = 4800;   // 0.1 s
        const auto pcm = make_pcm_ir(frames);
        SuperConvolverProcessor proc;
        proc.set_ir_pcm(pcm.data(), frames, 1, SR, "short");
        CHECK(proc.ir_pcm_frames() == frames);
    }

    SECTION("stereo planes are summed to mono at the source") {
        const std::size_t frames = 1000;
        std::vector<float> planar(frames * 2, 0.0f);
        for (std::size_t i = 0; i < frames; ++i) {
            planar[i] = 1.0f;                 // L
            planar[frames + i] = 3.0f;        // R
        }
        SuperConvolverProcessor proc;
        proc.set_ir_pcm(planar.data(), frames, 2, SR, "stereo");
        CHECK(proc.ir_pcm_frames() == frames);

        // One mono plane of the AVERAGE, so the blob is half the size and the
        // downstream mono-sum is a no-op rather than a second averaging pass.
        const auto blob = proc.serialize_plugin_state();
        const std::size_t payload =
            SuperConvolverProcessor::kStateHeaderSize + 1 + 8 + frames * sizeof(float);
        CHECK(blob.size() == payload);

        SuperConvolverProcessor restored;
        REQUIRE(restored.deserialize_plugin_state(
            std::span<const std::uint8_t>(blob)));
        CHECK(restored.ir_pcm_frames() == frames);
    }
}

// The built-in family is the zero-I/O IR source: every id must produce a real,
// distinct IR, and switching id must change what the audio path renders.
TEST_CASE("SuperConvolver built-in IRs are distinct and switchable",
          "[superconvolver][dsp][ir-source]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    REQUIRE(kBuiltInIrCount >= 3);

    std::vector<std::vector<float>> irs;
    for (std::uint8_t id = 0; id < kBuiltInIrCount; ++id) {
        pulp::state::StateStore store;
        SuperConvolverProcessor proc;
        configure(proc, store, /*size=*/0.5f, /*mix=*/100.0f, /*engine=*/0, /*rooms=*/1);
        proc.set_built_in_ir(id);
        proc.prepare(prep_ctx(SR, BLOCK));
        REQUIRE(proc.built_in_ir() == id);

        auto ir = proc.impulse_response_snapshot();
        INFO("built-in " << built_in_ir_name(id) << " len=" << ir.size());
        REQUIRE(ir.size() == static_cast<std::size_t>(0.5 * SR));
        CHECK(peak_of(ir) > 1e-3);                       // not silence
        for (float v : ir) REQUIRE(std::isfinite(v));    // and no NaN/Inf
        irs.push_back(std::move(ir));
        proc.release();
    }

    // Distinct spaces, not three names for one IR.
    for (std::size_t a = 0; a < irs.size(); ++a) {
        for (std::size_t b = a + 1; b < irs.size(); ++b) {
            std::vector<float> diff(irs[a].size());
            for (std::size_t i = 0; i < diff.size(); ++i) diff[i] = irs[a][i] - irs[b][i];
            const double rel = rms_from(diff, 0) / (rms_from(irs[a], 0) + 1e-12);
            INFO("built-in " << a << " vs " << b << " relative RMS difference = " << rel);
            CHECK(rel > 0.5);
        }
    }

    // Id 0 is the classic synthetic reverb — a preset written before the family
    // existed must still render exactly the IR it always did.
    const auto classic = pulp::examples::make_reverb_ir(static_cast<std::size_t>(0.5 * SR));
    CHECK(max_abs_diff(irs[0], classic) == 0.0);
}

// A Size change re-windows the cached decode; it must not re-run the decode +
// resample (the expensive half, and on the web lane a needless copy of a
// multi-megabyte buffer).
TEST_CASE("SuperConvolver Size re-windows a PCM IR without re-decoding",
          "[superconvolver][dsp][ir-source]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    const std::size_t frames = static_cast<std::size_t>(0.5 * SR);
    const auto pcm = make_pcm_ir(frames, 0x1357u);

    pulp::state::StateStore store;
    SuperConvolverProcessor proc;
    configure(proc, store, /*size=*/2.0f, /*mix=*/100.0f, /*engine=*/0, /*rooms=*/1);
    proc.set_background_worker_enabled(false);   // the web lane: no worker thread
    proc.set_ir_pcm(pcm.data(), pcm.size(), 1, SR, "unit-test");
    proc.prepare(prep_ctx(SR, BLOCK));

    REQUIRE(proc.source_decode_count() == 1);
    const std::size_t len0 = proc.impulse_response_snapshot().size();
    REQUIRE(len0 == frames);

    // Shorten Size well below the PCM length: the IR must be re-windowed...
    store.set_value(kSize, 0.2f);
    render_mono(proc, BLOCK, SR, 1, [](std::size_t) { return 0.0f; });
    proc.service_ir_rebuild();

    const std::size_t len1 = proc.impulse_response_snapshot().size();
    INFO("len0=" << len0 << " len1=" << len1
         << " decodes=" << proc.source_decode_count());
    CHECK(len1 == static_cast<std::size_t>(0.2 * SR));
    // ...from the cached decode, not a fresh one.
    CHECK(proc.source_decode_count() == 1);
    proc.release();
}

// ---------------------------------------------------------------------------
// State. v2 tags the IR source so a blob can carry a built-in id or the samples
// themselves — a raw filesystem path (all v1 could express) means nothing to a
// host with no filesystem. v1 and the pre-versioning raw-path blob must still
// load, or every existing native project loses its IR.
// ---------------------------------------------------------------------------
TEST_CASE("SuperConvolver SCv2 state round-trips every IR source kind",
          "[superconvolver][dsp][state]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    using Kind = SuperConvolverProcessor::IrStateKind;

    auto kind_of = [](const std::vector<std::uint8_t>& blob) {
        REQUIRE(blob.size() > SuperConvolverProcessor::kStateHeaderSize);
        REQUIRE(std::equal(SuperConvolverProcessor::kStateMagic,
                           SuperConvolverProcessor::kStateMagic + 4, blob.begin()));
        REQUIRE(blob[4] == SuperConvolverProcessor::kStateVersion);
        return static_cast<Kind>(blob[SuperConvolverProcessor::kStateHeaderSize]);
    };

    SECTION("kind 0 — synthetic default") {
        SuperConvolverProcessor p1;
        const auto blob = p1.serialize_plugin_state();
        CHECK(kind_of(blob) == Kind::Synthetic);

        SuperConvolverProcessor p2;
        CHECK(p2.deserialize_plugin_state(std::span<const std::uint8_t>(blob)));
        CHECK(p2.built_in_ir() == 0);
        CHECK(p2.ir_path().empty());
        CHECK(p2.ir_pcm_frames() == 0);
    }

    SECTION("kind 1 — built-in id") {
        SuperConvolverProcessor p1;
        p1.set_built_in_ir(2);
        const auto blob = p1.serialize_plugin_state();
        CHECK(kind_of(blob) == Kind::BuiltIn);

        SuperConvolverProcessor p2;
        CHECK(p2.deserialize_plugin_state(std::span<const std::uint8_t>(blob)));
        CHECK(p2.built_in_ir() == 2);
        CHECK(p2.ir_path().empty());
    }

    SECTION("kind 2 — inline IR PCM restores the same live IR") {
        const std::size_t frames = 2400;
        const auto pcm = make_pcm_ir(frames, 0x2468u);

        pulp::state::StateStore s1;
        SuperConvolverProcessor p1;
        configure(p1, s1, 2.0f, 100.0f, 0, 1);
        p1.set_ir_pcm(pcm.data(), pcm.size(), 1, SR, "inline");
        const auto blob = p1.serialize_plugin_state();
        CHECK(kind_of(blob) == Kind::Pcm);
        p1.prepare(prep_ctx(SR, BLOCK));
        const auto ir1 = p1.impulse_response_snapshot();
        p1.release();

        pulp::state::StateStore s2;
        SuperConvolverProcessor p2;
        configure(p2, s2, 2.0f, 100.0f, 0, 1);
        CHECK(p2.deserialize_plugin_state(std::span<const std::uint8_t>(blob)));
        CHECK(p2.ir_pcm_frames() == frames);
        p2.prepare(prep_ctx(SR, BLOCK));
        const auto ir2 = p2.impulse_response_snapshot();
        p2.release();

        REQUIRE(ir1.size() == ir2.size());
        CHECK(max_abs_diff(ir1, ir2) == 0.0);
    }

    SECTION("kind 3 — filesystem path (native)") {
        const std::string path = "/tmp/pulp_superconvolver_state_ir.wav";
        SuperConvolverProcessor p1;
        p1.set_ir_path(path);
        const auto blob = p1.serialize_plugin_state();
        CHECK(kind_of(blob) == Kind::Path);

        SuperConvolverProcessor p2;
        CHECK(p2.deserialize_plugin_state(std::span<const std::uint8_t>(blob)));
        CHECK(p2.ir_path() == path);
    }

    SECTION("SCv1 and pre-versioning raw-path blobs still restore their path") {
        const std::string path = "/tmp/pulp_superconvolver_legacy_ir.wav";

        std::vector<std::uint8_t> v1 = {'S', 'C', 'v', '1', 1};
        v1.insert(v1.end(), path.begin(), path.end());
        SuperConvolverProcessor p1;
        CHECK(p1.deserialize_plugin_state(std::span<const std::uint8_t>(v1)));
        CHECK(p1.ir_path() == path);

        const std::vector<std::uint8_t> raw(path.begin(), path.end());
        SuperConvolverProcessor p2;
        CHECK(p2.deserialize_plugin_state(std::span<const std::uint8_t>(raw)));
        CHECK(p2.ir_path() == path);

        SuperConvolverProcessor p3;
        CHECK(p3.deserialize_plugin_state(std::span<const std::uint8_t>()));
        CHECK(p3.ir_path().empty());
    }

    SECTION("a truncated v2 PCM payload falls back instead of failing the load") {
        std::vector<std::uint8_t> blob = {'S', 'C', 'v', '2', 2,
                                          static_cast<std::uint8_t>(Kind::Pcm)};
        blob.insert(blob.end(), {0x10, 0x00, 0x00, 0x00,   // frames = 16
                                 0x80, 0xBB, 0x00, 0x00}); // rate = 48000
        blob.push_back(0);  // ...and then nothing like 16 floats of samples

        SuperConvolverProcessor p;
        CHECK(p.deserialize_plugin_state(std::span<const std::uint8_t>(blob)));
        CHECK(p.ir_pcm_frames() == 0);
        CHECK(p.built_in_ir() == 0);
    }
}

// ---------------------------------------------------------------------------
// The thread-less (web) rebuild contract. Neither web lane can spawn a thread —
// the WAM link has no pthreads and the WCLAP hosts import no wasi.thread_spawn —
// so the background worker is disabled and the CONTROL thread pumps
// service_ir_rebuild(). process() must never rebuild: it only picks the finished
// IR up through the lock-free swapper.
// ---------------------------------------------------------------------------
TEST_CASE("SuperConvolver rebuilds only from service_ir_rebuild when the worker is off",
          "[superconvolver][dsp][ir-source][rt]") {
    constexpr double SR = 48000.0;
    constexpr std::size_t BLOCK = SuperConvolverProcessor::kInternalBlock;
    const std::size_t frames = 4800;
    const auto pcm = make_pcm_ir(frames, 0x99AAu);

    pulp::state::StateStore store;
    SuperConvolverProcessor proc;
    configure(proc, store, /*size=*/2.0f, /*mix=*/100.0f, /*engine=*/0, /*rooms=*/1);
    proc.set_background_worker_enabled(false);
    proc.prepare(prep_ctx(SR, BLOCK));

    // Starts on the default built-in IR.
    const auto ir_before = proc.impulse_response_snapshot();
    const std::uint32_t gen_before = proc.ir_generation();
    REQUIRE(peak_of(ir_before) > 1e-3);

    // Request a completely different IR, then run audio. With no worker thread and
    // no service call, process() must NOT rebuild — the live IR is unchanged and
    // the rendered output still matches the old IR exactly.
    proc.set_ir_pcm(pcm.data(), pcm.size(), 1, SR, "unit-test");
    const auto rendered_stale = measured_ir(proc, BLOCK, SR, frames);
    CHECK(proc.ir_generation() == gen_before);
    CHECK(proc.impulse_response_snapshot().size() == ir_before.size());
    CHECK(max_abs_diff(rendered_stale,
                       std::vector<float>(ir_before.begin(),
                                          ir_before.begin() +
                                              static_cast<std::ptrdiff_t>(frames)))
          < 1e-4);

    // The control thread services the rebuild; the audio thread picks the new IR
    // up on the next block boundary through the swapper.
    proc.service_ir_rebuild();
    CHECK(proc.ir_generation() != gen_before);
    const auto ir_after = proc.impulse_response_snapshot();
    REQUIRE(ir_after.size() == frames);
    CHECK(correlation(pcm, ir_after) > 0.999);

    const auto rendered_new = measured_ir(proc, BLOCK, SR, frames);
    INFO("max |rendered - new IR| = " << max_abs_diff(rendered_new, ir_after));
    CHECK(max_abs_diff(rendered_new, ir_after) < 1e-4);

    // And servicing again with nothing changed is a no-op (no rebuild churn).
    const std::uint32_t gen_after = proc.ir_generation();
    proc.service_ir_rebuild();
    CHECK(proc.ir_generation() == gen_after);
    proc.release();
}

// ---------------------------------------------------------------------------
// The host-driven non-RT pump (Processor::on_non_realtime_tick /
// non_realtime_tick_pending). This is what makes the Size knob work in the
// browser, where there is no worker thread to reconcile the IR: a WAM module
// lives entirely inside an AudioWorklet, and WebCLAP's only control context is
// the host's main-thread callback.
//
// The bug this pins: `requested_size_` is normally refreshed from the parameter
// store by process() — the audio thread is what sees host automation — so a
// control-thread tick that trusted that mirror reconciled the PREVIOUS Size and
// lagged one parameter change behind. In the browser that looked exactly like an
// inert knob: turn Size, nothing happens; turn it again, and you hear the size
// you asked for LAST time. The tick must read the store itself.
// ---------------------------------------------------------------------------
TEST_CASE("SuperConvolver rebuilds from a host tick with no process() in between",
          "[superconvolver][dsp][rt]") {
    constexpr double SR = 48000.0;
    constexpr int BLOCK = 512;

    pulp::state::StateStore store;
    SuperConvolverProcessor proc;
    configure(proc, store, /*size=*/2.0f, /*mix=*/100.0f, /*engine=*/0, /*rooms=*/1);
    proc.set_background_worker_enabled(false);   // the web lane: no worker thread
    proc.prepare(prep_ctx(SR, BLOCK));

    const std::size_t len_before = proc.impulse_response_snapshot().size();
    REQUIRE(len_before == static_cast<std::size_t>(2.0 * SR));
    CHECK_FALSE(proc.non_realtime_tick_pending());

    // Turn the Size knob. NOTHING else — no render block, so nothing has copied
    // the parameter into the processor's audio-thread mirror.
    store.set_value(kSize, 0.5f);

    // The processor must be able to say "I have work" from the parameter store
    // alone: that is the only thing CLAP's process() can consult before asking
    // the host for a main-thread callback.
    CHECK(proc.non_realtime_tick_pending());

    // One tick, straight from the control thread, with no process() call ever
    // having observed the new value. This is the literal browser sequence
    // (wam_set_param -> on_non_realtime_tick).
    proc.on_non_realtime_tick();

    const std::size_t len_after = proc.impulse_response_snapshot().size();
    INFO("len_before=" << len_before << " len_after=" << len_after);
    CHECK(len_after == static_cast<std::size_t>(0.5 * SR));   // NOT the old 2.0 s
    CHECK_FALSE(proc.non_realtime_tick_pending());            // and the work is done

    // Idempotent: a second tick with nothing changed rebuilds nothing.
    const std::uint32_t gen = proc.ir_generation();
    proc.on_non_realtime_tick();
    CHECK(proc.ir_generation() == gen);

    // A restored/parameter-driven source change is picked up by the same tick.
    proc.set_built_in_ir(2);
    CHECK(proc.non_realtime_tick_pending());
    proc.on_non_realtime_tick();
    CHECK(proc.ir_generation() != gen);
    CHECK_FALSE(proc.non_realtime_tick_pending());
    proc.release();
}

TEST_CASE("SuperConvolver's host tick defers to the background worker",
          "[superconvolver][dsp][rt]") {
    // With the worker running (the native default), the host must NOT be told
    // there is work pending and a stray tick must do nothing — the worker owns
    // the reconcile pass and the two must never run it concurrently.
    constexpr double SR = 48000.0;
    constexpr int BLOCK = 512;

    pulp::state::StateStore store;
    SuperConvolverProcessor proc;
    configure(proc, store, /*size=*/2.0f, /*mix=*/100.0f, /*engine=*/0, /*rooms=*/1);
    proc.prepare(prep_ctx(SR, BLOCK));   // worker enabled (default)

    store.set_value(kSize, 0.5f);
    CHECK_FALSE(proc.non_realtime_tick_pending());
    proc.release();
}

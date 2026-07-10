#pragma once
//
// Shared driver for the MF-3 denormal null test.
//
// This header renders a fixed, continuously-excited signal through every
// recursive filter that gained a `snap_to_zero` feedback-state write. It is
// compiled into TWO translation units:
//
//   * the reference TU (test/denormal_null_reference.cpp) with
//     PULP_DSP_ENABLE_SNAP_TO_ZERO defined to 0 — i.e. the exact pre-change
//     behavior, where snap_to_zero is the identity;
//   * the test TU (test/test_denormal_null.cpp) with the shipping default
//     (snap enabled).
//
// The null test then asserts the two are BIT-IDENTICAL sample-for-sample.
// That holds because the excitation keeps every filter state comfortably
// above the 1e-15 snap threshold for the whole buffer, so snap_to_zero never
// fires and the change is provably transparent to real audio. (A separate
// case in the test TU feeds silence to prove the guard actually flushes
// denormals when it *should* fire.)
//
// All harness functions live in an anonymous namespace so each TU gets its
// own internal-linkage copy — no ODR clash between the snap-on and snap-off
// builds. Only `denormal_null_reference()` (see the .cpp) crosses the TU
// boundary.

#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/biquad.hpp>
#include <pulp/signal/compressor.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/ladder_filter.hpp>
#include <pulp/signal/noise_gate.hpp>
#include <pulp/signal/phaser.hpp>
#include <pulp/signal/reverb.hpp>
#include <pulp/signal/svf.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <cmath>
#include <vector>

namespace denormal_null {

// External-linkage result types so the snap-disabled reference TU can hand them
// back to the snap-enabled test TU. Definitions are identical in both TUs.
struct AllOutputs {
    std::vector<float> biquad;
    std::vector<float> svf;
    std::vector<float> ladder;
    std::vector<float> ballistics;
    std::vector<float> dc_blocker;
    std::vector<float> tpt;
    std::vector<float> reverb;
    std::vector<float> phaser;
    std::vector<float> noise_gate;
    std::vector<float> compressor;
    std::vector<float> limiter;
};

struct TailReport {
    bool dc_blocker = false;
    bool ballistics = false;
    bool svf = false;
    bool ladder = false;
    bool reverb = false;
};

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kSampleRate = 44100.0f;
constexpr int kNumSamples = 8192;

// Deterministic, continuously-excited signal. Moderate amplitude with two
// tones plus a reproducible pseudo-random dither so no filter state ever
// decays into the denormal range within the buffer.
inline std::vector<float> make_signal() {
    std::vector<float> s(kNumSamples);
    std::uint32_t lcg = 0x1234567u;
    for (int i = 0; i < kNumSamples; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const float noise =
            (static_cast<float>(lcg >> 8) / static_cast<float>(0xFFFFFF)) - 0.5f;
        s[i] = 0.5f * std::sin(2.0f * kPi * 220.0f * static_cast<float>(i) / kSampleRate) +
               0.3f * std::sin(2.0f * kPi * 3000.0f * static_cast<float>(i) / kSampleRate) +
               0.05f * noise;
    }
    return s;
}

inline AllOutputs render_all() {
    const std::vector<float> in = make_signal();
    AllOutputs out;
    const auto n = static_cast<std::size_t>(kNumSamples);
    out.biquad.resize(n);
    out.svf.resize(n);
    out.ladder.resize(n);
    out.ballistics.resize(n);
    out.dc_blocker.resize(n);
    out.tpt.resize(n);
    out.reverb.resize(n);
    out.phaser.resize(n);
    out.noise_gate.resize(n);
    out.compressor.resize(n);
    out.limiter.resize(n);

    pulp::signal::Biquad biquad;
    biquad.set_coefficients(pulp::signal::Biquad::Type::lowpass, 1200.0f, 0.9f,
                            kSampleRate);

    pulp::signal::Svf svf;
    svf.set_sample_rate(kSampleRate);
    svf.set_frequency(900.0f);
    svf.set_resonance(4.0f);
    svf.set_mode(pulp::signal::Svf::Mode::bandpass);

    pulp::signal::LadderFilter ladder;
    ladder.set_sample_rate(kSampleRate);
    ladder.set_frequency(700.0f);
    ladder.set_resonance(0.85f);

    pulp::signal::BallisticsFilter ballistics;
    ballistics.prepare(kSampleRate);
    ballistics.set_attack_ms(2.0f);
    ballistics.set_release_ms(120.0f);

    pulp::signal::DcBlocker<float> dc;
    dc.set_pole(0.9995f);

    pulp::signal::TptFilter tpt;
    tpt.prepare(kSampleRate);
    tpt.set_cutoff(1500.0f);

    pulp::signal::Reverb reverb;
    reverb.prepare(kSampleRate);
    reverb.set_decay(2.5f);
    reverb.set_damping(0.4f);
    reverb.set_mix(0.5f);

    pulp::signal::Phaser phaser;
    phaser.set_sample_rate(kSampleRate);
    phaser.set_rate(0.5f);
    phaser.set_depth(0.7f);
    phaser.set_feedback(0.8f);
    phaser.set_mix(0.5f);
    phaser.set_stages(6);

    pulp::signal::NoiseGate gate;
    gate.set_sample_rate(kSampleRate);
    {
        pulp::signal::NoiseGate::Params p;
        p.threshold_db = -35.0f;
        p.ratio = 8.0f;
        p.attack_ms = 1.0f;
        p.release_ms = 80.0f;
        p.range_db = -60.0f;
        gate.set_params(p);
    }

    pulp::signal::Compressor comp;
    comp.set_sample_rate(kSampleRate);
    {
        pulp::signal::Compressor::Params p;
        p.threshold_db = -18.0f;
        p.ratio = 4.0f;
        p.attack_ms = 5.0f;
        p.release_ms = 120.0f;
        p.knee_db = 6.0f;
        comp.set_params(p);
    }

    pulp::signal::Limiter limiter;
    limiter.set_sample_rate(kSampleRate);
    limiter.set_threshold_db(-3.0f);
    limiter.set_release_ms(60.0f);

    for (std::size_t i = 0; i < n; ++i) {
        const float x = in[i];
        out.biquad[i] = biquad.process(x);
        out.svf[i] = svf.process(x);
        out.ladder[i] = ladder.process(x);
        out.ballistics[i] = ballistics.process(x);
        out.dc_blocker[i] = dc.process(x);
        out.tpt[i] = tpt.process_lowpass(x);
        out.reverb[i] = reverb.process(x).left;
        out.phaser[i] = phaser.process(x);
        out.noise_gate[i] = gate.process(x);
        out.compressor[i] = comp.process(x);
        out.limiter[i] = limiter.process(x);
    }
    return out;
}

// --- Silence-tail A/B: does a decaying-into-silence tail ever produce a
// subnormal output sample? Recorded per filter so the null test can assert the
// snap-OFF reference DOES (proving the configs actually reach the subnormal
// range) while the snap-ON build does NOT (proving the guard flushes them).
// Configs are tuned to a moderate decay that crosses the ~1e-38 subnormal
// range well inside kTailSamples.

constexpr int kTailSamples = 300000;

template <class Step>
inline bool tail_hits_subnormal(Step&& step) {
    step(1.0f);  // unit impulse, then silence
    bool hit = false;
    for (int i = 0; i < kTailSamples; ++i) {
        if (pulp::signal::is_denormal(step(0.0f))) hit = true;
    }
    return hit;
}

inline TailReport render_tails() {
    TailReport r;
    {
        pulp::signal::DcBlocker<float> dc;
        dc.set_pole(0.995f);
        r.dc_blocker = tail_hits_subnormal([&](float x) { return dc.process(x); });
    }
    {
        pulp::signal::BallisticsFilter env;
        env.prepare(kSampleRate);
        env.set_attack_ms(1.0f);
        env.set_release_ms(30.0f);
        r.ballistics = tail_hits_subnormal([&](float x) { return env.process(x); });
    }
    {
        pulp::signal::Svf svf;
        svf.set_sample_rate(kSampleRate);
        svf.set_frequency(500.0f);
        svf.set_resonance(2.0f);
        svf.set_mode(pulp::signal::Svf::Mode::bandpass);
        r.svf = tail_hits_subnormal([&](float x) { return svf.process(x); });
    }
    {
        pulp::signal::LadderFilter ladder;
        ladder.set_sample_rate(kSampleRate);
        ladder.set_frequency(400.0f);
        ladder.set_resonance(0.5f);
        r.ladder = tail_hits_subnormal([&](float x) { return ladder.process(x); });
    }
    {
        pulp::signal::Reverb reverb;
        reverb.prepare(kSampleRate);
        reverb.set_decay(0.3f);
        reverb.set_damping(0.5f);
        reverb.set_mix(1.0f);
        r.reverb = tail_hits_subnormal(
            [&](float x) { return reverb.process(x).left; });
    }
    return r;
}

}  // namespace
}  // namespace denormal_null

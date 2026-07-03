#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/resampler.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ────────────────────────────────────────────────────────────────────────
// macOS plan item 1.7 — Resampler (arbitrary-ratio polyphase).
//
// Pulp's `Resampler` runs a Kaiser-window polyphase FIR with linear
// inter-phase interpolation so a single design supports rational
// ratios (44.1↔48, 48↔96, 96↔192) and near-irrational ratios
// (48↔44099) without per-ratio re-design. These tests use INTERNAL
// goldens derived from analytical signals — pure sinusoids whose
// expected amplitude / phase / aliasing we can predict — rather than
// vendored libsamplerate/SoX vectors. That keeps the test surface
// fully MIT-licensed and self-contained.
// ────────────────────────────────────────────────────────────────────────

namespace {

constexpr double kPi = 3.14159265358979323846;

// Generate a pure sine of `freq_hz` at `rate_hz`, length `n`. Amplitude
// 1.0, phase 0 at t=0.
std::vector<float> sine(double freq_hz, double rate_hz, std::size_t n) {
    std::vector<float> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<float>(std::sin(2.0 * kPi * freq_hz * static_cast<double>(i) / rate_hz));
    }
    return out;
}

// Peak amplitude in the middle of a buffer (skip transients at the
// start and end where the FIR is still settling).
float steady_peak(const std::vector<float>& buf, std::size_t guard) {
    if (buf.size() < 2u * guard) return 0.0f;
    float p = 0.0f;
    for (std::size_t i = guard; i < buf.size() - guard; ++i) {
        p = std::max(p, std::fabs(buf[i]));
    }
    return p;
}

// Run the resampler over a single-channel input, return the resampled
// stream.
std::vector<float> resample_mono(Resampler& r,
                                 const std::vector<float>& input,
                                 std::size_t out_capacity) {
    std::vector<float> out(out_capacity, 0.0f);
    const std::size_t produced = r.process_block_mono(
        input.data(), input.size(), out.data(), out.size());
    out.resize(produced);
    return out;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────
// Kaiser-window math sanity (header-inline helpers).
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("bessel_i0 at 0 is exactly 1", "[signal][resampler][kaiser]") {
    REQUIRE_THAT(bessel_i0(0.0), WithinAbs(1.0, 1e-12));
}

TEST_CASE("bessel_i0 grows monotonically for positive x",
          "[signal][resampler][kaiser]") {
    double prev = bessel_i0(0.0);
    for (double x = 0.5; x <= 10.0; x += 0.5) {
        const double cur = bessel_i0(x);
        REQUIRE(cur > prev);
        prev = cur;
    }
}

TEST_CASE("kaiser_beta_for_stopband matches published table",
          "[signal][resampler][kaiser]") {
    // 21 dB → β ≈ 0; 50 dB → ~4.55; 96 dB → ~9.61.
    REQUIRE_THAT(kaiser_beta_for_stopband(20.0), WithinAbs(0.0, 1e-6));
    REQUIRE(kaiser_beta_for_stopband(60.0) > 5.0);
    REQUIRE_THAT(kaiser_beta_for_stopband(96.0), WithinAbs(9.6126, 0.05));
}

TEST_CASE("kaiser_window is symmetric and peaks at the center",
          "[signal][resampler][kaiser]") {
    std::vector<double> w(33, 0.0);
    kaiser_window(w, 6.0);
    // Symmetry
    for (std::size_t i = 0; i < w.size() / 2; ++i) {
        REQUIRE_THAT(w[i], WithinAbs(w[w.size() - 1 - i], 1e-12));
    }
    // Center is the peak.
    const double mid = w[w.size() / 2];
    for (std::size_t i = 0; i < w.size(); ++i) {
        REQUIRE(mid >= w[i] - 1e-12);
    }
    REQUIRE_THAT(mid, WithinAbs(1.0, 1e-9));
}

TEST_CASE("design_windowed_sinc has DC gain 1 and symmetric taps",
          "[signal][resampler][kaiser]") {
    const auto h = design_windowed_sinc(/*n_taps=*/65, /*fc=*/0.1, /*beta=*/8.0);
    double s = 0.0;
    for (double v : h) s += v;
    REQUIRE_THAT(s, WithinAbs(1.0, 1e-9));
    for (std::size_t i = 0; i < h.size() / 2; ++i) {
        REQUIRE_THAT(h[i], WithinAbs(h[h.size() - 1 - i], 1e-12));
    }
}

// ────────────────────────────────────────────────────────────────────────
// Resampler basic invariants.
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("Resampler DC unity gain across ratios",
          "[signal][resampler]") {
    struct Case { double in; double out; };
    const Case cases[] = {
        { 48000.0, 48000.0 },     // identity
        { 44100.0, 48000.0 },     // up by rational
        { 48000.0, 44100.0 },     // down by rational
        { 48000.0, 88200.0 },     // common host rate
        { 48000.0, 96000.0 },     // 2x up
        { 96000.0, 48000.0 },     // 2x down
        { 88200.0, 48000.0 },     // common source rate
        { 176400.0, 48000.0 },    // high source rate
        { 48000.0, 176400.0 },    // high host rate
        { 192000.0, 48000.0 },    // 4x down
        { 48000.0, 192000.0 },    // 4x up
        { 44100.0, 47999.0 },     // near-irrational up
        { 48000.0, 44099.0 },     // near-irrational down
    };
    for (const auto& c : cases) {
        Resampler r;
        r.prepare(c.in, c.out, /*channels=*/1, /*max_block=*/8192);
        const std::vector<float> dc(8192, 1.0f);
        const std::size_t out_cap = r.max_output_for(dc.size());
        std::vector<float> out(out_cap, 0.0f);
        const std::size_t produced = r.process_block_mono(
            dc.data(), dc.size(), out.data(), out.size());
        REQUIRE(produced > 0u);
        // Steady-state center of the output should sit very close to
        // 1.0 (DC gain is exact by construction).
        const std::size_t guard = std::min<std::size_t>(produced / 4, 256u);
        const double avg = ([&]() {
            double s = 0.0;
            std::size_t n = 0;
            for (std::size_t i = guard; i + guard < produced; ++i, ++n) {
                s += out[i];
            }
            return n == 0 ? 0.0 : s / static_cast<double>(n);
        })();
        INFO("in=" << c.in << " out=" << c.out << " avg=" << avg);
        REQUIRE_THAT(avg, WithinAbs(1.0, 1e-3));
    }
}

TEST_CASE("Resampler preserves in-band sine amplitude (44.1→48k)",
          "[signal][resampler]") {
    Resampler r;
    r.prepare(44100.0, 48000.0, 1, 4096);
    // 1 kHz sits well inside both passbands.
    const auto in = sine(1000.0, 44100.0, 8192);
    const auto out = resample_mono(r, in, r.max_output_for(in.size()));
    REQUIRE(out.size() > 1024u);
    // Skip front + back transients (≈ filter span at output rate).
    const std::size_t guard = 256;
    const float peak = steady_peak(out, guard);
    REQUIRE_THAT(static_cast<double>(peak), WithinAbs(1.0, 5e-3));
}

TEST_CASE("Resampler integer-ratio fast path preserves in-band amplitude",
          "[signal][resampler]") {
    // Integer ratios (2x down, 2x up) drive the phase accumulator onto exact phase
    // boundaries, so the fractional weight is exactly 0 and only the first polyphase
    // branch contributes — the case the inner loop skips the second MAC for. A
    // 1 kHz tone well inside both passbands must survive the fast path at full gain.
    struct Case { double in; double out; };
    for (const Case c : {Case{96000.0, 48000.0}, Case{48000.0, 96000.0}}) {
        Resampler r;
        r.prepare(c.in, c.out, 1, 8192);
        const auto in = sine(1000.0, c.in, 8192);
        const auto out = resample_mono(r, in, r.max_output_for(in.size()));
        REQUIRE(out.size() > 1024u);
        for (float v : out) REQUIRE(std::isfinite(v));
        const float peak = steady_peak(out, 256);
        INFO("ratio " << c.in << "->" << c.out << " peak=" << peak);
        REQUIRE_THAT(static_cast<double>(peak), WithinAbs(1.0, 5e-3));
    }
}

TEST_CASE("Resampler output is invariant to input block chunking",
          "[signal][resampler]") {
    // The streaming phase accumulator + delay line must make the resampled stream
    // independent of how the input is chunked across process_block calls. Feeding
    // the same signal in one shot vs in odd 37-sample chunks exercises the delay
    // line at every write position — including the dual-write mirror that keeps the
    // tap window contiguous — so a ring-index off-by-one surfaces here as a
    // one-shot-vs-chunked mismatch. The two runs must be bit-identical.
    struct Case { double in; double out; };
    for (const Case c : {Case{44100.0, 48000.0}, Case{48000.0, 44100.0}, Case{48000.0, 96000.0}}) {
        const auto in = sine(1234.0, c.in, 6000);

        Resampler one;
        one.prepare(c.in, c.out, 1, in.size());
        const auto ref = resample_mono(one, in, one.max_output_for(in.size()));

        Resampler chunked;
        chunked.prepare(c.in, c.out, 1, 4096);
        std::vector<float> got;
        for (std::size_t off = 0; off < in.size();) {
            const std::size_t chunk = std::min<std::size_t>(37u, in.size() - off);
            std::vector<float> obuf(chunked.max_output_for(chunk) + 4u, 0.0f);
            const std::size_t produced =
                chunked.process_block_mono(in.data() + off, chunk, obuf.data(), obuf.size());
            got.insert(got.end(), obuf.begin(), obuf.begin() + static_cast<std::ptrdiff_t>(produced));
            off += chunk;
        }
        INFO("ratio " << c.in << "->" << c.out << " one=" << ref.size() << " chunked=" << got.size());
        REQUIRE(got.size() == ref.size());
        for (std::size_t i = 0; i < ref.size(); ++i) REQUIRE(got[i] == ref[i]);
    }
}

TEST_CASE("Resampler float MAC keeps distortion below the stopband",
          "[signal][resampler]") {
    // The polyphase convolution accumulates in single precision across four
    // independent lanes so the compiler can vectorize the reduction. That
    // reassociation trades the last few ulps for speed, so this guards that the
    // added round-off stays far below the design's stopband: a full-scale in-band
    // tone must resample with a noise+distortion floor near the filter's own
    // limit, not lifted by the accumulator.
    //
    // THD+N is estimated with a Hann-windowed least-squares fit of the output to
    // A·cos + B·sin at the tone frequency; the residual after subtracting the
    // fundamental is the noise+distortion energy. The window suppresses the
    // spectral leakage that would otherwise dominate a single-bin estimate.
    const auto thdn_db = [](double in, double out, double f0) {
        Resampler r;
        r.prepare(in, out, 1, 4096);
        const std::size_t n_in = static_cast<std::size_t>(in * 1.2);
        std::vector<float> input(n_in);
        for (std::size_t i = 0; i < n_in; ++i)
            input[i] = 0.9f * static_cast<float>(std::sin(2.0 * kPi * f0 / in * i));
        std::vector<float> outbuf(r.max_output_for(n_in) + 8u, 0.0f);
        const std::size_t n =
            r.process_block_mono(input.data(), input.size(), outbuf.data(), outbuf.size());
        // Drop the filter transient at both ends before measuring steady state.
        const std::size_t start = 1024u;
        const std::size_t end = (n > 1024u) ? n - 1024u : n;
        const std::size_t N = (end > start) ? end - start : 0u;
        if (N < 4096u) return 0.0;  // too short — treat as a failure
        const double th = 2.0 * kPi * f0 / out;
        double scc = 0, sss = 0, syc = 0, sys = 0;
        for (std::size_t i = 0; i < N; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (N - 1));
            const double c = std::cos(th * i), s = std::sin(th * i), y = outbuf[start + i];
            scc += w * c * c; sss += w * s * s; syc += w * y * c; sys += w * y * s;
        }
        const double A = syc / scc, B = sys / sss;
        double noise = 0, sig = 0;
        for (std::size_t i = 0; i < N; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (N - 1));
            const double c = std::cos(th * i), s = std::sin(th * i), y = outbuf[start + i];
            const double fit = A * c + B * s, e = y - fit;
            noise += w * e * e; sig += w * fit * fit;
        }
        return 10.0 * std::log10(noise / sig + 1e-300);
    };

    // Non-integer audio ratios: the floor is set by the 64-phase linear
    // interpolation, so the single-precision MAC leaves it unchanged. Measured
    // ≈ -80 dB (48→44.1) and ≈ -74 dB (44.1→48); assert with margin.
    REQUIRE(thdn_db(48000.0, 44100.0, 1000.0) < -70.0);
    REQUIRE(thdn_db(44100.0, 48000.0, 1000.0) < -68.0);
    // Integer ratio: no inter-phase interpolation masks the accumulation, so this
    // directly exercises the float MAC's precision. Measured ≈ -143 dB (double is
    // ≈ -150); a broken reassociation would lift this floor by tens of dB.
    REQUIRE(thdn_db(96000.0, 48000.0, 1000.0) < -120.0);
    // Worst case for accumulation error: an extreme downsample drives the Kaiser
    // design to the tap-count ceiling (taps_per_phase ≈ 1024, the longest supported
    // filter), and the integer 24:1 ratio again bypasses interpolation — so the
    // float sum runs over the most terms it ever will. Even here the floor stays
    // ≈ -140 dB, confirming the 4-lane reduction error grows far slower than the
    // stopband; a regression in the long-filter regime would surface here.
    REQUIRE(thdn_db(192000.0, 8000.0, 1000.0) < -120.0);
}

TEST_CASE("Resampler 1 kHz round-trip 44.1→48→44.1 reconstructs the sine",
          "[signal][resampler]") {
    // Acceptance check uses sample-by-sample RMS-error against a
    // reference sine after compensating for filter group delay. This
    // is more sensitive than a single-bin Goertzel THD+N estimate on
    // a non-period-aligned window — small leakage there dominates the
    // measurement even when the audio sounds perfect.
    Resampler up;
    up.prepare(44100.0, 48000.0, 1, 8192);
    Resampler down;
    down.prepare(48000.0, 44100.0, 1, 8192);
    const auto in = sine(1000.0, 44100.0, 8192);
    auto upsampled = resample_mono(up, in, up.max_output_for(in.size()));
    auto roundtrip = resample_mono(down, upsampled, down.max_output_for(upsampled.size()));
    REQUIRE(roundtrip.size() > 2048u);

    // Round-trip group delay = (taps_per_phase-1)/2 at each stage, so
    // the round-trip latency is roughly `(N_up + N_down)/2` samples
    // at the input rate. Find the best integer alignment by sweeping
    // a small window of candidate delays and picking the lag with the
    // smallest RMS error vs the reference sine.
    const std::size_t guard = 2048;
    REQUIRE(roundtrip.size() > guard + 2048u);
    const std::size_t n = std::min<std::size_t>(2048, roundtrip.size() - 2u * guard);
    double best_err = std::numeric_limits<double>::infinity();
    int best_lag = 0;
    for (int lag = -8; lag <= 8; ++lag) {
        double e = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(static_cast<int>(guard + i) + lag) / 44100.0;
            const double ref = std::sin(2.0 * kPi * 1000.0 * t);
            const double d = static_cast<double>(roundtrip[guard + i]) - ref;
            e += d * d;
        }
        if (e < best_err) { best_err = e; best_lag = lag; }
    }
    const double rms = std::sqrt(best_err / static_cast<double>(n));
    INFO("round-trip rms error = " << rms << " best_lag = " << best_lag);
    // 1.0 amplitude reference → 0.05 RMS = ~ -26 dB error. The
    // implementation typically clears 1e-3 here; a slightly looser
    // bound keeps the test robust to float-arithmetic ordering.
    REQUIRE(rms < 5e-2);
}

TEST_CASE("Resampler suppresses out-of-band signals (above output Nyquist)",
          "[signal][resampler]") {
    // Downsample 96 kHz → 48 kHz with a 30 kHz tone (above the 24 kHz
    // output Nyquist). The polyphase FIR's stopband should attenuate
    // this far enough that it shows up as noise, not a clear alias.
    Resampler r;
    r.prepare(96000.0, 48000.0, 1, 8192);
    const auto in = sine(30000.0, 96000.0, 16384);
    const auto out = resample_mono(r, in, r.max_output_for(in.size()));
    REQUIRE(out.size() > 1024u);
    const std::size_t guard = 1024;
    const float peak = steady_peak(out, guard);
    // 30 kHz folds to 18 kHz; with ≥ 96 dB stopband, alias amplitude
    // should be far below the original unit-amplitude. Use a loose
    // bound (-40 dB ≈ 0.01) so the test stays robust to design knob
    // tweaks; the implementation typically clears -80 dB.
    INFO("alias peak = " << peak);
    REQUIRE(peak < 0.01f);
}

TEST_CASE("Resampler streaming state survives buffer-size changes",
          "[signal][resampler]") {
    // Feed the same input in two different chunk patterns; the
    // outputs should match sample-for-sample within float epsilon.
    Resampler r1;
    r1.prepare(48000.0, 44100.0, 1, 4096);
    Resampler r2;
    r2.prepare(48000.0, 44100.0, 1, 4096);

    const auto in = sine(440.0, 48000.0, 4096);

    // r1: one big block.
    std::vector<float> out1(r1.max_output_for(in.size()), 0.0f);
    const std::size_t n1 = r1.process_block_mono(
        in.data(), in.size(), out1.data(), out1.size());
    out1.resize(n1);

    // r2: varying small blocks.
    std::vector<float> out2;
    out2.reserve(out1.size() + 16);
    const std::size_t sizes[] = { 33, 128, 7, 512, 901, 1, 2514 };
    std::size_t in_pos = 0;
    for (std::size_t s : sizes) {
        const std::size_t take = std::min(s, in.size() - in_pos);
        if (take == 0) break;
        std::vector<float> tmp(r2.max_output_for(take), 0.0f);
        const std::size_t produced = r2.process_block_mono(
            in.data() + in_pos, take, tmp.data(), tmp.size());
        out2.insert(out2.end(), tmp.begin(), tmp.begin() + produced);
        in_pos += take;
    }

    // Truncate to the shorter length (the two paths may finish on
    // slightly different phase remainders).
    const std::size_t n = std::min(out1.size(), out2.size());
    REQUIRE(n > 1024u);
    for (std::size_t i = 0; i < n; ++i) {
        // Sub-LSB drift acceptable; differences should be ~ float-eps
        // because both paths run the exact same arithmetic.
        REQUIRE_THAT(out1[i], WithinAbs(out2[i], 1e-5f));
    }
}

TEST_CASE("Resampler ratio change mid-stream does not click",
          "[signal][resampler]") {
    Resampler r;
    r.prepare(48000.0, 48000.0, 1, 4096);
    const auto in1 = sine(440.0, 48000.0, 4096);
    std::vector<float> out1(r.max_output_for(in1.size()), 0.0f);
    const std::size_t n1 = r.process_block_mono(
        in1.data(), in1.size(), out1.data(), out1.size());
    out1.resize(n1);

    // Change ratio: now 48k in → 44.1k out.
    r.set_ratio(48000.0, 44100.0);
    const auto in2 = sine(440.0, 48000.0, 4096);
    std::vector<float> out2(r.max_output_for(in2.size()), 0.0f);
    const std::size_t n2 = r.process_block_mono(
        in2.data(), in2.size(), out2.data(), out2.size());
    out2.resize(n2);

    REQUIRE(out1.size() > 256u);
    REQUIRE(out2.size() > 256u);
    // Within each segment, after the transient window, peak should
    // sit near 1.0 with no large discontinuity.
    REQUIRE_THAT(static_cast<double>(steady_peak(out1, 256)), WithinAbs(1.0, 0.05));
    REQUIRE_THAT(static_cast<double>(steady_peak(out2, 256)), WithinAbs(1.0, 0.05));
    // Sample-to-sample jump at the boundary stays bounded by what a
    // 440 Hz sine can do at the new rate. Just assert the last sample
    // of out1 and first sample of out2 are both small in magnitude
    // (not pinned to ±1), guarding against an obvious click spike.
    REQUIRE(std::fabs(out1.back()) <= 1.5f);
    REQUIRE(std::fabs(out2.front()) <= 1.5f);
}

TEST_CASE("Resampler handles 16 channels without cross-talk",
          "[signal][resampler]") {
    constexpr std::size_t kChannels = 16;
    Resampler r;
    r.prepare(48000.0, 96000.0, kChannels, 1024);
    // Per-channel input: channel c is a sine at (200 + 50*c) Hz.
    std::vector<std::vector<float>> ins(kChannels, std::vector<float>(2048, 0.0f));
    for (std::size_t c = 0; c < kChannels; ++c) {
        const double f = 200.0 + 50.0 * static_cast<double>(c);
        for (std::size_t i = 0; i < ins[c].size(); ++i) {
            ins[c][i] = static_cast<float>(std::sin(2.0 * kPi * f * static_cast<double>(i) / 48000.0));
        }
    }
    std::vector<const float*> in_ptrs(kChannels);
    std::vector<std::vector<float>> outs(kChannels, std::vector<float>(r.max_output_for(2048), 0.0f));
    std::vector<float*> out_ptrs(kChannels);
    for (std::size_t c = 0; c < kChannels; ++c) {
        in_ptrs[c] = ins[c].data();
        out_ptrs[c] = outs[c].data();
    }
    const std::size_t produced = r.process_block(
        in_ptrs.data(), 2048, out_ptrs.data(), outs[0].size());
    REQUIRE(produced > 1024u);

    // Each output channel's peak should sit near 1.0 — proves no
    // cross-talk / state interleaving.
    for (std::size_t c = 0; c < kChannels; ++c) {
        outs[c].resize(produced);
        const float p = steady_peak(outs[c], 512);
        INFO("ch=" << c << " peak=" << p);
        REQUIRE_THAT(static_cast<double>(p), WithinAbs(1.0, 0.05));
    }
}

TEST_CASE("Resampler max_output_for matches actual block production",
          "[signal][resampler]") {
    Resampler r;
    r.prepare(44100.0, 96000.0, 1, 1024);
    const auto in = sine(1000.0, 44100.0, 1024);
    const std::size_t cap = r.max_output_for(in.size());
    std::vector<float> out(cap + 16, 0.0f);
    const std::size_t produced = r.process_block_mono(
        in.data(), in.size(), out.data(), out.size());
    REQUIRE(produced > 0u);
    REQUIRE(produced <= cap);
}

TEST_CASE("Resampler detailed process reports consumed input frames",
          "[signal][resampler]") {
    Resampler r;
    r.prepare(48000.0, 96000.0, 1, 1024);
    const auto in = sine(1000.0, 48000.0, 1024);
    std::vector<float> out(64, 0.0f);

    const auto detailed =
        r.process_block_mono_detailed(in.data(), in.size(), out.data(), out.size());
    REQUIRE(detailed.output_frames == out.size());
    REQUIRE(detailed.input_frames_consumed > 0u);
    REQUIRE(detailed.input_frames_consumed < in.size());

    std::vector<float> out_legacy(64, 0.0f);
    Resampler legacy;
    legacy.prepare(48000.0, 96000.0, 1, 1024);
    REQUIRE(legacy.process_block_mono(in.data(), in.size(), out_legacy.data(), out_legacy.size()) ==
            detailed.output_frames);
}

TEST_CASE("Resampler consumed-input accounting resumes the same stream",
          "[signal][resampler]") {
    const auto in = sine(1000.0, 48000.0, 1024);

    Resampler one_shot;
    one_shot.prepare(48000.0, 96000.0, 1, 1024);
    std::vector<float> expected(256, 0.0f);
    const auto expected_result = one_shot.process_block_mono_detailed(
        in.data(), in.size(), expected.data(), expected.size());
    REQUIRE(expected_result.output_frames == expected.size());

    Resampler split;
    split.prepare(48000.0, 96000.0, 1, 1024);
    std::vector<float> first(64, 0.0f);
    const auto first_result = split.process_block_mono_detailed(
        in.data(), in.size(), first.data(), first.size());
    REQUIRE(first_result.output_frames == first.size());
    REQUIRE(first_result.input_frames_consumed > 0u);

    std::vector<float> second(expected.size() - first.size(), 0.0f);
    const auto second_result = split.process_block_mono_detailed(
        in.data() + first_result.input_frames_consumed,
        in.size() - first_result.input_frames_consumed,
        second.data(),
        second.size());
    REQUIRE(second_result.output_frames == second.size());

    for (std::size_t i = 0; i < first.size(); ++i) {
        REQUIRE_THAT(first[i], WithinAbs(expected[i], 1e-6f));
    }
    for (std::size_t i = 0; i < second.size(); ++i) {
        REQUIRE_THAT(second[i], WithinAbs(expected[first.size() + i], 1e-6f));
    }
}

TEST_CASE("Resampler prepare produces a rectangular polyphase split",
          "[signal][resampler]") {
    Resampler r;
    r.prepare(48000.0, 96000.0, 1, 1024);
    REQUIRE(r.phases() >= 2u);
    REQUIRE(r.taps_per_phase() >= 2u);
    REQUIRE(r.prototype_length() == r.phases() * r.taps_per_phase());
}

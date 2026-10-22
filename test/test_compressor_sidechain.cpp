#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/compressor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }

float rms(const std::vector<float>& v, int start, int end) {
    double acc = 0.0;
    const int n = end - start;
    for (int i = start; i < end; ++i) acc += double(v[i]) * double(v[i]);
    return static_cast<float>(std::sqrt(acc / n));
}

} // namespace

TEST_CASE("Compressor latency_samples reflects lookahead",
          "[signal][compressor]") {
    Compressor c;
    c.set_sample_rate(48000.0f);
    REQUIRE(c.latency_samples() == 0);

    c.set_lookahead_ms(1.0f);
    REQUIRE(c.latency_samples() == 48);

    c.set_lookahead_ms(5.0f);
    REQUIRE(c.latency_samples() == 240);

    // Clamp at 50 ms max.
    c.set_lookahead_ms(1000.0f);
    REQUIRE(c.lookahead_ms() == 50.0f);
    REQUIRE(c.latency_samples() == 2400);
}

TEST_CASE("Compressor with lookahead delays the dry signal by the same amount",
          "[signal][compressor]") {
    Compressor c;
    c.set_sample_rate(48000.0f);
    c.set_lookahead_ms(1.0f); // 48 samples
    Compressor::Params p; p.threshold_db = 0.0f; // no actual compression
    c.set_params(p);

    // Drive with an impulse at sample 0.
    std::vector<float> buf(256, 0.0f);
    buf[0] = 1.0f;
    c.process(buf.data(), 256);

    // With lookahead = 48 samples, the impulse should now appear at
    // sample ~48 in the output (the dry signal was delayed by the
    // lookahead amount).
    int found = -1;
    for (int i = 0; i < 256; ++i) {
        if (std::abs(buf[i]) > 0.5f) { found = i; break; }
    }
    REQUIRE(found == 48);
}

TEST_CASE("Compressor sidechain HPF attenuates bass-driven triggering",
          "[signal][compressor]") {
    // A loud sub-bass sidechain signal should NOT cause gain reduction
    // when the HPF is set above the sub-bass.
    Compressor c;
    c.set_sample_rate(48000.0f);
    c.set_sidechain_hpf_hz(200.0f);
    Compressor::Params p;
    p.threshold_db = -20.0f;
    p.ratio = 8.0f;
    p.attack_ms = 0.1f;
    p.release_ms = 50.0f;
    c.set_params(p);

    const int n = 4096;
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kSub = 60.0f; // well below the 200 Hz HPF corner

    // Build a hot 60 Hz sidechain.
    std::vector<float> sidechain(n);
    for (int i = 0; i < n; ++i)
        sidechain[i] = 0.8f * std::sin(kTwoPi * kSub * float(i) / 48000.0f);

    // Process a constant 0.5 input against the bass sidechain.
    std::vector<float> input(n, 0.5f);
    c.process_with_sidechain(input.data(), sidechain.data(), n);

    // With HPF on, the sidechain detector sees a much-attenuated bass,
    // so envelope_db should hover close to 0 → almost no gain reduction
    // → output ≈ 0.5.
    const float tail_rms = rms(input, n / 2, n);
    REQUIRE(tail_rms > 0.45f); // very little compression
    REQUIRE(c.gain_reduction_db() > -3.0f); // shallow at worst
}

TEST_CASE("Compressor sidechain WITHOUT HPF clearly compresses bass-driven input",
          "[signal][compressor]") {
    Compressor c;
    c.set_sample_rate(48000.0f);
    c.set_sidechain_hpf_hz(0.0f); // HPF disabled
    Compressor::Params p;
    p.threshold_db = -20.0f;
    p.ratio = 8.0f;
    p.attack_ms = 0.1f;
    p.release_ms = 50.0f;
    c.set_params(p);

    const int n = 4096;
    constexpr float kTwoPi = 6.28318530717958647692f;
    constexpr float kSub = 60.0f;

    std::vector<float> sidechain(n);
    for (int i = 0; i < n; ++i)
        sidechain[i] = 0.8f * std::sin(kTwoPi * kSub * float(i) / 48000.0f);

    std::vector<float> input(n, 0.5f);
    c.process_with_sidechain(input.data(), sidechain.data(), n);

    // Without HPF the bass triggers compression → audible gain reduction.
    REQUIRE(c.gain_reduction_db() < -3.0f);
}

TEST_CASE("Compressor process(input) still works as a self-sidechain compressor",
          "[signal][compressor]") {
    // Regression test: the original single-arg `process(input)` API
    // must keep working with input acting as its own sidechain detector.
    Compressor c;
    c.set_sample_rate(48000.0f);
    Compressor::Params p;
    p.threshold_db = -20.0f;
    p.ratio = 4.0f;
    p.attack_ms = 5.0f;
    p.release_ms = 50.0f;
    c.set_params(p);

    // Drive with a -10 dBFS sine (above threshold) and assert gain
    // reduction kicks in.
    constexpr float kTwoPi = 6.28318530717958647692f;
    const float amp = db_to_lin(-10.0f);
    std::vector<float> buf(4096);
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = amp * std::sin(kTwoPi * 1000.0f * float(i) / 48000.0f);
    c.process(buf.data(), static_cast<int>(buf.size()));

    REQUIRE(c.gain_reduction_db() < -1.0f);
}

TEST_CASE("Compressor reset clears sidechain HPF + lookahead state",
          "[signal][compressor]") {
    Compressor c;
    c.set_sample_rate(48000.0f);
    c.set_sidechain_hpf_hz(150.0f);
    c.set_lookahead_ms(2.0f);
    Compressor::Params p; p.threshold_db = -20.0f;
    c.set_params(p);

    // Pump some signal through to populate the delay + filter state.
    for (int i = 0; i < 512; ++i) (void) c.process(0.5f);

    c.reset();
    // After reset, processing a zero impulse stream should produce
    // zeros for at least the lookahead window (delay line cleared).
    bool all_zero = true;
    for (int i = 0; i < c.latency_samples(); ++i) {
        if (std::abs(c.process(0.0f)) > 1e-9f) { all_zero = false; break; }
    }
    REQUIRE(all_zero);
}

namespace {

std::vector<float> loud_program(std::size_t n, float sample_rate) {
    // Alternating loud and quiet sections so the envelope both attacks and
    // releases, with LCG noise so no two blocks repeat.
    std::vector<float> s(n);
    std::uint32_t lcg = 0x9e3779bu;
    for (std::size_t i = 0; i < n; ++i) {
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = static_cast<float>(lcg >> 8) / 16777216.0f - 0.5f;
        const float t = static_cast<float>(i) / sample_rate;
        const float level = (i / 2048) % 2 == 0 ? 1.0f : 0.05f;
        s[i] = level * (0.6f * std::sin(6.2831853f * 220.0f * t) +
                        0.25f * std::sin(6.2831853f * 3150.0f * t) + 0.15f * noise);
    }
    return s;
}

std::vector<int> uneven_blocks(std::size_t total) {
    static constexpr int kPattern[] = {1, 63, 64, 65, 7, 128, 333, 1024, 2, 512, 31};
    std::vector<int> blocks;
    std::size_t done = 0;
    for (std::size_t k = 0; done < total; ++k) {
        const int b = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(kPattern[k % std::size(kPattern)]), total - done));
        blocks.push_back(b);
        done += static_cast<std::size_t>(b);
    }
    return blocks;
}

struct CompressorCase {
    float threshold_db, ratio, attack_ms, release_ms, knee_db, makeup_db, hpf_hz, lookahead_ms;
};

constexpr CompressorCase kCompressorCases[] = {
    {-20.0f, 4.0f, 5.0f, 100.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {-30.0f, 8.0f, 0.5f, 50.0f, 6.0f, 3.0f, 0.0f, 0.0f},
    {-12.0f, 2.0f, 20.0f, 400.0f, 12.0f, -2.0f, 120.0f, 0.0f},
    {-24.0f, 20.0f, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f, 5.0f},
    {-18.0f, 3.0f, 1.0f, 200.0f, 3.0f, 6.0f, 80.0f, 2.0f},
};

void configure(Compressor& c, const CompressorCase& k, float sample_rate) {
    c.set_sample_rate(sample_rate);
    Compressor::Params p;
    p.threshold_db = k.threshold_db;
    p.ratio = k.ratio;
    p.attack_ms = k.attack_ms;
    p.release_ms = k.release_ms;
    p.knee_db = k.knee_db;
    p.makeup_db = k.makeup_db;
    c.set_params(p);
    c.set_sidechain_hpf_hz(k.hpf_hz);
    c.set_lookahead_ms(k.lookahead_ms);
}

// The compressor's algorithm with libm log10/pow in float, sample by sample:
// the reference the branch-free dB conversions are held to. Everything but the
// two conversions matches the shipping arithmetic, so the residual isolates
// them.
std::vector<float> reference_compress(const std::vector<float>& input, const CompressorCase& k,
                                      float sample_rate, CompressorBallistics ballistics) {
    auto coeff = [&](float ms) {
        return ms <= 0.0f ? 1.0f : 1.0f - std::exp(-1.0f / (ms * 0.001f * sample_rate));
    };
    const float attack = coeff(k.attack_ms);
    const float release = coeff(k.release_ms);
    const float slope = 1.0f / k.ratio - 1.0f;
    auto curve = [&](float in_db) {
        const float thr = k.threshold_db;
        const float above = (in_db - thr) * slope;
        if (k.knee_db <= 0.0f)
            return in_db <= thr ? 0.0f : above;
        const float half = k.knee_db / 2.0f;
        if (in_db < thr - half)
            return 0.0f;
        if (in_db > thr + half)
            return above;
        const float x = in_db - thr + half;
        return slope * x * x / (2.0f * k.knee_db);
    };
    Biquad hpf;
    if (k.hpf_hz > 0.0f)
        hpf.set_coefficients(Biquad::Type::highpass, k.hpf_hz, 0.7071068f, sample_rate);
    const auto delay = static_cast<std::size_t>(std::round(k.lookahead_ms * 0.001f * sample_rate));
    std::vector<float> out(input.size());
    float env = 0.0f;
    float hold = 0.0f;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const float det = k.hpf_hz > 0.0f ? hpf.process(input[i]) : input[i];
        const float in_db = 20.0f * std::log10(std::max(std::abs(det), 1e-10f));
        const float g = curve(in_db);
        if (ballistics == CompressorBallistics::decoupled) {
            // Peak hold of the (negative) gain change, then attack smoothing.
            const float released = hold + release * (g - hold);
            hold = g < released ? g : released;
            env = env + attack * (hold - env);
        } else {
            env = env + (g < env ? attack : release) * (g - env);
        }
        const float gain = std::pow(10.0f, (env + k.makeup_db) / 20.0f);
        const float dry = delay > 0 ? (i >= delay ? input[i - delay] : 0.0f) : input[i];
        out[i] = dry * gain;
    }
    return out;
}

} // namespace

TEST_CASE("Compressor block path is bit-identical to the sample path across split blocks",
          "[signal][compressor][determinism]") {
    for (const auto detector : {CompressorBallistics::branching, CompressorBallistics::decoupled})
        for (const auto& k : kCompressorCases) {
            INFO("detector " << static_cast<int>(detector) << " threshold " << k.threshold_db
                             << " knee " << k.knee_db << " hpf " << k.hpf_hz << " lookahead "
                             << k.lookahead_ms);
            const auto input = loud_program(16384, 48000.0f);
            Compressor by_sample;
            Compressor by_block;
            configure(by_sample, k, 48000.0f);
            configure(by_block, k, 48000.0f);
            by_sample.set_ballistics(detector);
            by_block.set_ballistics(detector);
            auto expected = input;
            for (float& x : expected)
                x = by_sample.process(x);
            auto actual = input;
            std::size_t pos = 0;
            for (int block : uneven_blocks(input.size())) {
                by_block.process(actual.data() + pos, block);
                pos += static_cast<std::size_t>(block);
            }
            CHECK(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(float)) == 0);
            CHECK(by_block.gain_reduction_db() == by_sample.gain_reduction_db());
        }
}

TEST_CASE("Compressor output stays within -120 dBFS of the libm conversions",
          "[signal][compressor][precision]") {
    for (const auto ballistics : {CompressorBallistics::decoupled, CompressorBallistics::branching})
        for (const float sr : {44100.0f, 48000.0f, 96000.0f}) {
            for (const auto& k : kCompressorCases) {
                INFO("ballistics " << static_cast<int>(ballistics) << " sr " << sr << " threshold "
                                   << k.threshold_db << " knee " << k.knee_db);
                const auto input = loud_program(16384, sr);
                Compressor c;
                configure(c, k, sr);
                c.set_ballistics(ballistics);
                auto output = input;
                c.process(output.data(), static_cast<int>(output.size()));
                const auto reference = reference_compress(input, k, sr, ballistics);
                double worst = 0.0;
                for (std::size_t i = 0; i < output.size(); ++i)
                    worst = std::max(worst, std::abs(static_cast<double>(output[i]) -
                                                     static_cast<double>(reference[i])));
                CHECK(worst < 1.0e-6);
            }
        }
}

TEST_CASE("Compressor ballistic coefficients follow parameter and rate changes",
          "[signal][compressor]") {
    auto expected = [](float ms, float sr) { return 1.0f - std::exp(-1.0f / (ms * 0.001f * sr)); };
    Compressor c;
    c.set_sample_rate(48000.0f);
    REQUIRE(c.attack_coefficient() == expected(5.0f, 48000.0f));
    REQUIRE(c.release_coefficient() == expected(100.0f, 48000.0f));

    Compressor::Params p;
    p.attack_ms = 2.0f;
    p.release_ms = 250.0f;
    c.set_params(p);
    REQUIRE(c.attack_coefficient() == expected(2.0f, 48000.0f));
    REQUIRE(c.release_coefficient() == expected(250.0f, 48000.0f));

    c.set_sample_rate(96000.0f);
    REQUIRE(c.attack_coefficient() == expected(2.0f, 96000.0f));
    REQUIRE(c.release_coefficient() == expected(250.0f, 96000.0f));

    p.attack_ms = 0.0f;
    c.set_params(p);
    REQUIRE(c.attack_coefficient() == 1.0f);
}

TEST_CASE("Compressor steady-state gain reduction follows the static curve",
          "[signal][compressor]") {
    // A constant detector level settles the envelope on the curve value.
    for (const float knee : {0.0f, 6.0f}) {
        for (const float level_db : {-40.0f, -23.0f, -20.0f, -17.0f, -6.0f, 0.0f}) {
            INFO("knee " << knee << " level " << level_db);
            Compressor c;
            c.set_sample_rate(48000.0f);
            Compressor::Params p;
            p.threshold_db = -20.0f;
            p.ratio = 4.0f;
            p.attack_ms = 1.0f;
            p.release_ms = 5.0f;
            p.knee_db = knee;
            c.set_params(p);
            std::vector<float> dc(48000, db_to_lin(level_db));
            c.process(dc.data(), static_cast<int>(dc.size()));
            double expected = 0.0;
            const double slope = 1.0 / 4.0 - 1.0;
            if (knee <= 0.0f) {
                expected = level_db <= -20.0f ? 0.0 : (level_db + 20.0) * slope;
            } else if (level_db > -20.0f + knee / 2) {
                expected = (level_db + 20.0) * slope;
            } else if (level_db >= -20.0f - knee / 2) {
                const double x = level_db + 20.0 + knee / 2;
                expected = slope * x * x / (2.0 * knee);
            }
            CHECK(std::abs(static_cast<double>(c.gain_reduction_db()) - expected) < 1.0e-4);
            CHECK(std::abs(static_cast<double>(dc.back()) -
                           static_cast<double>(db_to_lin(level_db)) *
                               std::pow(10.0, expected / 20.0)) < 1.0e-6);
        }
    }
}

TEST_CASE("Compressor block path rejects non-finite samples at the exact sample",
          "[signal][compressor]") {
    auto input = loud_program(1024, 48000.0f);
    input[100] = std::numeric_limits<float>::quiet_NaN();
    input[700] = std::numeric_limits<float>::infinity();
    Compressor by_sample;
    Compressor by_block;
    configure(by_sample, kCompressorCases[4], 48000.0f);
    configure(by_block, kCompressorCases[4], 48000.0f);
    auto expected = input;
    for (float& x : expected)
        x = by_sample.process(x);
    auto actual = input;
    by_block.process(actual.data(), static_cast<int>(actual.size()));
    REQUIRE(actual[100] == 0.0f);
    REQUIRE(actual[700] == 0.0f);
    for (float x : actual)
        REQUIRE(std::isfinite(x));
    CHECK(std::memcmp(actual.data(), expected.data(), actual.size() * sizeof(float)) == 0);
}

namespace {

// 10%-90% time (ms) of a gain-reduction transition from `from` to `to`.
double transition_ms(const std::vector<float>& gr, std::size_t begin, std::size_t end, double from,
                     double to, double sample_rate) {
    const double low = from + 0.1 * (to - from);
    const double high = from + 0.9 * (to - from);
    long t_low = -1;
    for (std::size_t i = begin; i < end; ++i) {
        const double v = gr[i];
        if (t_low < 0 && (v - low) * (to - from) >= 0.0)
            t_low = static_cast<long>(i);
        if ((v - high) * (to - from) >= 0.0)
            return static_cast<double>(static_cast<long>(i) - t_low) * 1000.0 / sample_rate;
    }
    return -1.0;
}

struct StepTiming {
    double attack_ms, release_ms, settled_db;
};

// A -40 -> -6 dBFS step and back, at threshold -20 dB, 4:1, hard knee, as a
// steady level (tone_hz 0) or a sine of tone_hz.
StepTiming step_timing(CompressorBallistics detector, float attack_ms, float release_ms,
                       double sample_rate, double tone_hz) {
    Compressor c;
    c.set_sample_rate(static_cast<float>(sample_rate));
    Compressor::Params p;
    p.threshold_db = -20.0f;
    p.ratio = 4.0f;
    p.attack_ms = attack_ms;
    p.release_ms = release_ms;
    p.knee_db = 0.0f;
    c.set_params(p);
    c.set_ballistics(detector);
    const auto n = static_cast<std::size_t>(sample_rate * 1.2);
    const auto on = static_cast<std::size_t>(sample_rate * 0.2);
    const auto off = static_cast<std::size_t>(sample_rate * 0.7);
    std::vector<float> gr(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double level = (i >= on && i < off) ? std::pow(10.0, -6.0 / 20.0) : 0.01;
        const double x = tone_hz > 0.0 ? level * std::sin(2.0 * std::numbers::pi * tone_hz *
                                                          static_cast<double>(i) / sample_rate)
                                       : level;
        (void)c.process(static_cast<float>(x));
        gr[i] = c.gain_reduction_db();
    }
    const double settled = gr[off - 1];
    return {transition_ms(gr, on, off, 0.0, settled, sample_rate),
            transition_ms(gr, off, n, settled, 0.0, sample_rate), settled};
}

// THD+N of a compressed -6 dBFS tone: everything but a least-squares fit of
// the fundamental, relative to it, after the detector has settled.
double tone_thd_n_db(CompressorBallistics detector, double tone_hz) {
    constexpr double kSr = 48000.0;
    Compressor c;
    c.set_sample_rate(static_cast<float>(kSr));
    Compressor::Params p;
    p.threshold_db = -20.0f;
    p.ratio = 4.0f;
    p.attack_ms = 5.0f;
    p.release_ms = 100.0f;
    p.knee_db = 0.0f;
    c.set_params(p);
    c.set_ballistics(detector);
    std::vector<float> y(static_cast<std::size_t>(kSr));
    for (std::size_t i = 0; i < y.size(); ++i)
        y[i] = static_cast<float>(
            0.5 * std::sin(2.0 * std::numbers::pi * tone_hz * static_cast<double>(i) / kSr));
    c.process(y.data(), static_cast<int>(y.size()));
    const std::size_t begin = y.size() / 2;
    double ss = 0, sc = 0, cc = 0, ys = 0, yc = 0;
    for (std::size_t i = begin; i < y.size(); ++i) {
        const double w = 2.0 * std::numbers::pi * tone_hz * static_cast<double>(i) / kSr;
        const double s = std::sin(w), co = std::cos(w);
        ss += s * s;
        cc += co * co;
        sc += s * co;
        ys += y[i] * s;
        yc += y[i] * co;
    }
    const double det = ss * cc - sc * sc;
    const double a = (ys * cc - yc * sc) / det, b = (yc * ss - ys * sc) / det;
    double fundamental = 0, residual = 0;
    for (std::size_t i = begin; i < y.size(); ++i) {
        const double w = 2.0 * std::numbers::pi * tone_hz * static_cast<double>(i) / kSr;
        const double f = a * std::sin(w) + b * std::cos(w);
        fundamental += f * f;
        residual += (y[i] - f) * (y[i] - f);
    }
    return 10.0 * std::log10(residual / fundamental);
}

} // namespace

TEST_CASE("Compressor attack and release are one-pole time constants on a level step",
          "[signal][compressor][timing]") {
    // A steady-level step moves from 10% to 90% in ln(9) tau = 2.197 tau for
    // either detector.
    for (const auto detector : {CompressorBallistics::branching, CompressorBallistics::decoupled})
        for (const double sr : {44100.0, 96000.0})
            for (const auto [attack, release] : {std::pair{5.0f, 100.0f}, std::pair{1.0f, 50.0f}}) {
                INFO("detector " << static_cast<int>(detector) << " sr " << sr << " attack "
                                 << attack << " release " << release);
                const auto t = step_timing(detector, attack, release, sr, 0.0);
                CHECK_THAT(t.attack_ms, WithinRel(2.197 * attack, 0.02));
                CHECK_THAT(t.release_ms, WithinRel(2.197 * release, 0.02));
                CHECK_THAT(t.settled_db, WithinAbs(-10.5, 0.01));
            }
}

TEST_CASE("Compressor decoupled detector tracks the attack knob on a tone",
          "[signal][compressor][timing]") {
    // On a 1 kHz tone the branching detector rides the waveform: attack about
    // twice the knob and settling short of the static curve. The decoupled
    // detector holds each cycle's peak, so it matches the level step.
    const auto branching =
        step_timing(CompressorBallistics::branching, 5.0f, 100.0f, 48000.0, 1000.0);
    const auto decoupled =
        step_timing(CompressorBallistics::decoupled, 5.0f, 100.0f, 48000.0, 1000.0);
    // The figures the signal guide and the migration note quote.
    CHECK_THAT(branching.attack_ms, WithinRel(21.5, 0.02));
    CHECK_THAT(branching.settled_db, WithinAbs(-9.75, 0.02));
    CHECK_THAT(decoupled.attack_ms, WithinRel(11.0, 0.02));
    CHECK_THAT(decoupled.release_ms, WithinRel(2.197 * 100.0, 0.02));
    CHECK_THAT(decoupled.settled_db, WithinAbs(-10.49, 0.02));
}

TEST_CASE("Compressor decoupled detector distorts low tones less",
          "[signal][compressor][distortion]") {
    // Gain that moves within each cycle is harmonic distortion. Measured at
    // 5/100 ms: branching -39.5 / -45.5 dB THD+N at 50 / 100 Hz, decoupled
    // -48.8 / -60.3 dB.
    for (const double hz : {50.0, 100.0}) {
        INFO(hz << " Hz");
        const double branching = tone_thd_n_db(CompressorBallistics::branching, hz);
        const double decoupled = tone_thd_n_db(CompressorBallistics::decoupled, hz);
        CHECK(decoupled < branching - 8.0);
    }
    // The THD+N table the signal guide and the migration note quote, within
    // half a decibel.
    struct Row {
        double hz, branching_db, decoupled_db;
    };
    for (const Row row :
         {Row{50.0, -39.5, -48.8}, Row{100.0, -45.5, -60.3}, Row{1000.0, -65.5, -99.8}}) {
        INFO(row.hz << " Hz");
        CHECK_THAT(tone_thd_n_db(CompressorBallistics::branching, row.hz),
                   WithinAbs(row.branching_db, 0.5));
        CHECK_THAT(tone_thd_n_db(CompressorBallistics::decoupled, row.hz),
                   WithinAbs(row.decoupled_db, 0.5));
    }
}

TEST_CASE("Compressor defaults to the branching ballistics", "[signal][compressor][compat]") {
    // The default keeps the previous detector, so existing products and
    // saved patches render exactly as before; decoupled is opt-in.
    const auto input = loud_program(16384, 48000.0f);
    for (const auto& k : kCompressorCases) {
        Compressor by_default;
        Compressor explicit_branching;
        configure(by_default, k, 48000.0f);
        configure(explicit_branching, k, 48000.0f);
        explicit_branching.set_ballistics(CompressorBallistics::branching);
        CHECK(by_default.ballistics() == CompressorBallistics::branching);
        auto a = input;
        auto b = input;
        by_default.process(a.data(), static_cast<int>(a.size()));
        explicit_branching.process(b.data(), static_cast<int>(b.size()));
        CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
    }
    CHECK(CompressorBallistics{} == CompressorBallistics::branching);
}

TEST_CASE("Compressor decoupled release is the release hold and attack pole in series",
          "[signal][compressor][timing]") {
    // With attack comparable to release the decoupled form releases slower
    // than 2.197 tau, because the release hold is filtered again by the
    // attack one-pole; branching releases at 2.197 tau. Measured 10%-90%
    // release at 48 kHz: 30/60 ms 155.4 ms, 50/50 ms 167.9 ms.
    struct Case {
        float attack, release;
        double decoupled_release_ms;
    };
    for (const Case c : {Case{30.0f, 60.0f, 155.4}, Case{50.0f, 50.0f, 167.9}}) {
        INFO("attack " << c.attack << " release " << c.release);
        const auto branching =
            step_timing(CompressorBallistics::branching, c.attack, c.release, 48000.0, 0.0);
        const auto decoupled =
            step_timing(CompressorBallistics::decoupled, c.attack, c.release, 48000.0, 0.0);
        CHECK_THAT(branching.attack_ms, WithinRel(2.197 * c.attack, 0.02));
        CHECK_THAT(branching.release_ms, WithinRel(2.197 * c.release, 0.02));
        CHECK_THAT(decoupled.attack_ms, WithinRel(2.197 * c.attack, 0.02));
        CHECK_THAT(decoupled.release_ms, WithinRel(c.decoupled_release_ms, 0.02));
        CHECK(decoupled.release_ms > 1.15 * branching.release_ms);
    }
}

TEST_CASE("Compressor set_ballistics with the current form leaves state untouched",
          "[signal][compressor][compat]") {
    // Hosts select the form every block; re-selecting must not reseed the
    // decoupled peak hold.
    const auto input = loud_program(8192, 48000.0f);
    Compressor once;
    Compressor every_block;
    for (auto* c : {&once, &every_block}) {
        configure(*c, kCompressorCases[0], 48000.0f);
        c->set_ballistics(CompressorBallistics::decoupled);
    }
    auto a = input;
    auto b = input;
    for (std::size_t pos = 0; pos < input.size(); pos += 256) {
        once.process(a.data() + pos, 256);
        every_block.set_ballistics(CompressorBallistics::decoupled);
        every_block.process(b.data() + pos, 256);
    }
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

TEST_CASE("Compressor gain computer and ballistics are callable without audio",
          "[signal][compressor][pipeline]") {
    // A widget draws the transfer curve from gain_computer_db(); it must be
    // the curve process() settles on, for hard and soft knees.
    for (const float knee : {0.0f, 6.0f, 12.0f}) {
        Compressor::Params p;
        p.threshold_db = -24.0f;
        p.ratio = 5.0f;
        p.knee_db = knee;
        p.attack_ms = 1.0f;
        p.release_ms = 20.0f;
        for (float level_db = -60.0f; level_db <= 0.0f; level_db += 3.0f) {
            INFO("knee " << knee << " level " << level_db);
            Compressor c;
            c.set_sample_rate(48000.0f);
            c.set_params(p);
            const float x = std::pow(10.0f, level_db / 20.0f);
            for (int i = 0; i < 48000; ++i)
                (void)c.process(x);
            const float curve = Compressor::gain_computer_db(Compressor::detector_level_db(x), p);
            CHECK_THAT(c.gain_reduction_db(), WithinAbs(curve, 1e-4));
        }
    }
    // The steps the two ballistics run, as pure functions.
    const float attack = Compressor::ballistic_coefficient(5.0f, 48000.0f);
    const float release = Compressor::ballistic_coefficient(100.0f, 48000.0f);
    CHECK(Compressor::ballistic_coefficient(0.0f, 48000.0f) == 1.0f);
    CHECK(Compressor::branching_step(0.0f, -10.0f, attack, release) == attack * -10.0f);
    CHECK(Compressor::branching_step(-10.0f, 0.0f, attack, release) == -10.0f + release * 10.0f);
    // The hold jumps to deeper reduction and releases toward shallower.
    CHECK(Compressor::decoupled_hold(-2.0f, -10.0f, release) == -10.0f);
    CHECK(Compressor::decoupled_hold(-10.0f, 0.0f, release) == -10.0f + release * 10.0f);
}

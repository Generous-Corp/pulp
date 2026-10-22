#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/compressor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

using namespace pulp::signal;

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
                                      float sample_rate) {
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
    for (std::size_t i = 0; i < input.size(); ++i) {
        const float det = k.hpf_hz > 0.0f ? hpf.process(input[i]) : input[i];
        const float in_db = 20.0f * std::log10(std::max(std::abs(det), 1e-10f));
        const float g = curve(in_db);
        env = env + (g < env ? attack : release) * (g - env);
        const float gain = std::pow(10.0f, (env + k.makeup_db) / 20.0f);
        const float dry = delay > 0 ? (i >= delay ? input[i - delay] : 0.0f) : input[i];
        out[i] = dry * gain;
    }
    return out;
}

} // namespace

TEST_CASE("Compressor block path is bit-identical to the sample path across split blocks",
          "[signal][compressor][determinism]") {
    for (const auto& k : kCompressorCases) {
        INFO("threshold " << k.threshold_db << " knee " << k.knee_db << " hpf " << k.hpf_hz
                          << " lookahead " << k.lookahead_ms);
        const auto input = loud_program(16384, 48000.0f);
        Compressor by_sample;
        Compressor by_block;
        configure(by_sample, k, 48000.0f);
        configure(by_block, k, 48000.0f);
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
    for (const float sr : {44100.0f, 48000.0f, 96000.0f}) {
        for (const auto& k : kCompressorCases) {
            INFO("sr " << sr << " threshold " << k.threshold_db << " knee " << k.knee_db);
            const auto input = loud_program(16384, sr);
            Compressor c;
            configure(c, k, sr);
            auto output = input;
            c.process(output.data(), static_cast<int>(output.size()));
            const auto reference = reference_compress(input, k, sr);
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

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/ladder_bank.hpp>
#include <pulp/signal/ladder_filter.hpp>
#include <pulp/signal/svf.hpp>
#include <pulp/signal/svf_bank.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include "support/denormal_tail.hpp"
#include "support/filter_block_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

// TptFilter (sample and block paths) and the lockstep voice banks
// (LadderBank, SvfBank) against the scalar filters they must reproduce.

using namespace pulp::signal;
using namespace pulp::test::filter_block;
using Catch::Matchers::WithinAbs;

// ── TptFilter ────────────────────────────────────────────────────────────────

TEST_CASE("TptFilter owns invalid sample-rate normalization", "[signal][filter][tpt]") {
    for (const float sample_rate : {
             0.0f,
             -1.0f,
             std::numeric_limits<float>::quiet_NaN(),
             std::numeric_limits<float>::infinity(),
         }) {
        TptFilter filter;
        filter.prepare(sample_rate);
        filter.set_cutoff(1.0f);

        REQUIRE(filter.sample_rate() == TptFilter::kMinSampleRate);
        REQUIRE(filter.cutoff() == TptFilter::kMinCutoffHz);
        REQUIRE(std::isfinite(filter.process_lowpass(0.25f)));
    }

    TptFilter64 filter64;
    filter64.prepare(-std::numeric_limits<double>::infinity());
    filter64.set_cutoff(1.0);
    REQUIRE(filter64.sample_rate() == TptFilter64::kMinSampleRate);
    REQUIRE(std::isfinite(filter64.process_lowpass(0.25)));
}

TEST_CASE("TptFilter reclamps its retained cutoff when the rate falls", "[signal][filter][tpt]") {
    TptFilter filter;
    filter.prepare(48000.0f);
    filter.set_cutoff(10000.0f);

    filter.prepare(TptFilter::kMinSampleRate);

    REQUIRE_THAT(filter.cutoff(),
                 WithinAbs(TptFilter::kMinSampleRate * TptFilter::kMaxCutoffFraction, 1.0e-6f));
    REQUIRE(std::isfinite(filter.process_lowpass(0.25f)));
}

TEST_CASE("TptFilter64 calculates coefficients at double precision",
          "[signal][filter][tpt][precision]") {
    constexpr double sample_rate = 192000.0;
    constexpr double cutoff = 12345.678901234;
    TptFilter64 filter;
    filter.prepare(sample_rate);
    filter.set_cutoff(cutoff);

    const double tangent = std::tan(std::numbers::pi_v<double> * cutoff / sample_rate);
    const double expected_first_sample = tangent / (1.0 + tangent);

    REQUIRE_THAT(filter.process_lowpass(1.0), WithinAbs(expected_first_sample, 1.0e-15));
}

TEST_CASE("TptFilter block paths are bit-identical to the sample paths across split blocks",
          "[signal][filter][tpt][determinism]") {
    constexpr float sr = 48000.0f;
    const auto input = filters_busy_signal(8192, sr);
    const auto schedule = filters_split_schedule(input.size());

    for (const float cutoff : {20.0f, 1000.0f, 0.24f * sr, 0.45f * sr}) {
        INFO("cutoff " << cutoff);
        auto run = [&](auto sample_fn, auto block_fn) {
            TptFilter by_sample;
            TptFilter by_block;
            by_sample.prepare(sr);
            by_block.prepare(sr);
            std::vector<float> expected = input;
            std::vector<float> actual = input;
            std::size_t pos = 0;
            int k = 0;
            for (int block : schedule) {
                // Modulate the cutoff at every block boundary on both paths.
                const float c = cutoff * (1.0f + 0.25f * static_cast<float>(k++ % 3));
                by_sample.set_cutoff(c);
                by_block.set_cutoff(c);
                for (int i = 0; i < block; ++i)
                    expected[pos + static_cast<std::size_t>(i)] =
                        sample_fn(by_sample, expected[pos + static_cast<std::size_t>(i)]);
                block_fn(by_block, actual.data() + pos, block);
                pos += static_cast<std::size_t>(block);
            }
            return same_bits(actual, expected);
        };
        CHECK(run([](TptFilter& f, float x) { return f.process_lowpass(x); },
                  [](TptFilter& f, float* p, int n) { f.process_lowpass(p, n); }));
        CHECK(run([](TptFilter& f, float x) { return f.process_highpass(x); },
                  [](TptFilter& f, float* p, int n) { f.process_highpass(p, n); }));
        CHECK(run([](TptFilter& f, float x) { return f.process_allpass(x); },
                  [](TptFilter& f, float* p, int n) { f.process_allpass(p, n); }));
        CHECK(run([](TptFilter& f, float x) { return f.process(x).lowpass; },
                  [](TptFilter& f, float* p, int n) { f.process_lowpass(p, n); }));
    }
}

TEST_CASE("TptFilter tracks a double-precision trapezoidal reference",
          "[signal][filter][tpt][precision]") {
    // The recursion is s' = s + 2g (x - s) with lowpass g (x - s) + s; this
    // is the same filter evaluated in double. The float path must stay within
    // a few float ulps of full scale of it, which bounds any rearrangement of
    // the update to well below -120 dBFS.
    for (const float sr : {44100.0f, 48000.0f, 96000.0f}) {
        for (const float cutoff : {20.0f, 1000.0f, 0.24f * sr, 0.45f * sr}) {
            INFO("sr " << sr << " cutoff " << cutoff);
            TptFilter filter;
            filter.prepare(sr);
            filter.set_cutoff(cutoff);
            const double g = std::tan(std::numbers::pi_v<double> * static_cast<double>(cutoff) /
                                      static_cast<double>(sr));
            const double gd = g / (1.0 + g);
            const auto input = filters_busy_signal(16384, sr);
            auto output = input;
            filter.process_lowpass(output.data(), static_cast<int>(output.size()));
            double state = 0.0;
            double worst = 0.0;
            for (std::size_t i = 0; i < input.size(); ++i) {
                const double d = static_cast<double>(input[i]) - state;
                const double lp = gd * d + state;
                state += 2.0 * gd * d;
                worst = std::max(worst, std::abs(lp - static_cast<double>(output[i])));
            }
            CHECK(worst < 1.0e-6);
        }
    }
}

TEST_CASE("TptFilter block path flushes a decaying tail without subnormal residency",
          "[signal][filter][tpt][denormal]") {
    using namespace pulp::test::denormal_tail;
    if (flush_to_zero_active())
        SKIP("this thread flushes subnormals to zero; the tail counts would be vacuous");
    constexpr std::size_t kTotal = std::size_t{1} << 18;
    constexpr float sr = 48000.0f;

    // Negative control: the unsnapped recursion at 5 Hz sits in a subnormal
    // limit cycle for the rest of the buffer.
    TptFilter probe;
    probe.prepare(sr);
    probe.set_cutoff(5.0f);
    const float g = probe.process_lowpass(1.0f); // first lowpass sample == g
    float state = 0.0f;
    const auto control = impulse_tail(
        [&](float* x, int n) {
            for (int i = 0; i < n; ++i) {
                const float d = x[i] - state;
                x[i] = g * d + state;
                state = 2.0f * g * d + state;
            }
        },
        kTotal, 512);
    REQUIRE(control.subnormal_outputs > kTotal / 2);
    REQUIRE(control.nonzero_end == kTotal);

    for (std::size_t block : {std::size_t{1}, std::size_t{64}, std::size_t{4096}, kTotal}) {
        INFO("block " << block);
        TptFilter filter;
        filter.prepare(sr);
        filter.set_cutoff(5.0f);
        const auto tail =
            impulse_tail([&](float* x, int n) { filter.process_lowpass(x, n); }, kTotal, block);
        REQUIRE(tail.subnormal_outputs == 0);
        REQUIRE(tail.nonzero_end > 0);
        REQUIRE(tail.nonzero_end < kTotal / 4);
    }

    // Near a quarter of the sample rate the pole is close to zero; at most one
    // chunk of subnormal output before exact zero.
    TptFilter fast;
    fast.prepare(sr);
    fast.set_cutoff(0.2f * sr);
    const auto tail =
        impulse_tail([&](float* x, int n) { fast.process_lowpass(x, n); }, kTotal, kTotal);
    REQUIRE(tail.subnormal_outputs <= pulp::signal::kSnapChunkSamples);
    REQUIRE(tail.nonzero_end < 1024);
}

// ── Voice banks ─────────────────────────────────────────────────────────────

TEST_CASE("LadderBank voices are bit-identical to independent LadderFilters",
          "[signal][ladder][bank][determinism]") {
    constexpr std::size_t kVoices = 16;
    LadderBank<kVoices> bank;
    std::array<LadderFilter, kVoices> scalar;
    bank.set_sample_rate(48000.0f);
    for (std::size_t v = 0; v < kVoices; ++v) {
        scalar[v].set_sample_rate(48000.0f);
        scalar[v].set_frequency(300.0f + 250.0f * static_cast<float>(v));
        scalar[v].set_resonance(0.1f * static_cast<float>(v % 11));
        bank.set_frequency(v, 300.0f + 250.0f * static_cast<float>(v));
        bank.set_resonance(v, 0.1f * static_cast<float>(v % 11));
    }
    std::size_t mismatches = 0;
    for (int n = 0; n < 12000; ++n) {
        if (n == 4000) {
            // Retune and reset single voices mid-stream on both sides.
            scalar[3].set_frequency(5000.0f);
            bank.set_frequency(3, 5000.0f);
            scalar[7].set_resonance(1.0f);
            bank.set_resonance(7, 1.0f);
            scalar[9].reset();
            bank.reset(9);
        }
        std::array<float, kVoices> in{};
        for (std::size_t v = 0; v < kVoices; ++v)
            in[v] = 2.5f * std::sin(0.013f * static_cast<float>(n) * static_cast<float>(v + 1));
        std::array<float, kVoices> out{};
        bank.process(in.data(), out.data());
        for (std::size_t v = 0; v < kVoices; ++v) {
            const float expected = scalar[v].process(in[v]);
            mismatches += std::memcmp(&expected, &out[v], sizeof(float)) != 0;
        }
    }
    CHECK(mismatches == 0);
    CHECK(bank.stage_coefficient(3) == LadderFilter::stage_coefficient(5000.0f, 48000.0f));
}

TEST_CASE("LadderBank set_sample_rate recomputes every voice's coefficient at once",
          "[signal][ladder][bank]") {
    constexpr std::size_t kVoices = 8;
    LadderBank<kVoices> bank;
    for (std::size_t v = 0; v < kVoices; ++v)
        bank.set_frequency(v, 500.0f + 700.0f * static_cast<float>(v));
    // A rate change after the frequencies applies without another
    // frequency or resonance change.
    bank.set_sample_rate(96000.0f);
    CHECK(bank.sample_rate() == 96000.0f);
    for (std::size_t v = 0; v < kVoices; ++v)
        CHECK(bank.stage_coefficient(v) ==
              LadderFilter::stage_coefficient(500.0f + 700.0f * static_cast<float>(v), 96000.0f));

    // And it matches a scalar filter configured rate-then-frequency, sample
    // for sample.
    LadderFilter scalar;
    scalar.set_sample_rate(96000.0f);
    scalar.set_frequency(500.0f + 700.0f * 5.0f);
    std::size_t mismatches = 0;
    for (int n = 0; n < 2000; ++n) {
        std::array<float, kVoices> frame{};
        frame.fill(std::sin(0.03f * static_cast<float>(n)));
        bank.process(frame.data(), frame.data());
        const float expected = scalar.process(std::sin(0.03f * static_cast<float>(n)));
        mismatches += std::memcmp(&expected, &frame[5], sizeof(float)) != 0;
    }
    CHECK(mismatches == 0);
}

TEST_CASE("LadderBank process_voices matches per-sample bank processing",
          "[signal][ladder][bank]") {
    constexpr std::size_t kVoices = 4;
    LadderBank<kVoices> by_block;
    LadderBank<kVoices> by_sample;
    for (auto* b : {&by_block, &by_sample}) {
        b->set_sample_rate(44100.0f);
        for (std::size_t v = 0; v < kVoices; ++v) {
            b->set_frequency(v, 800.0f * static_cast<float>(v + 1));
            b->set_resonance(v, 0.6f);
        }
    }
    std::array<std::vector<float>, kVoices> buffers;
    std::array<float*, kVoices> pointers{};
    for (std::size_t v = 0; v < kVoices; ++v) {
        buffers[v].resize(500);
        for (std::size_t n = 0; n < 500; ++n)
            buffers[v][n] = std::sin(0.02f * static_cast<float>(n * (v + 1)));
        pointers[v] = buffers[v].data();
    }
    auto expected = buffers;
    by_block.process_voices(pointers.data(), 500);
    for (std::size_t n = 0; n < 500; ++n) {
        std::array<float, kVoices> frame{};
        for (std::size_t v = 0; v < kVoices; ++v)
            frame[v] = expected[v][n];
        by_sample.process(frame.data(), frame.data());
        for (std::size_t v = 0; v < kVoices; ++v)
            expected[v][n] = frame[v];
    }
    for (std::size_t v = 0; v < kVoices; ++v)
        CHECK(std::memcmp(buffers[v].data(), expected[v].data(), 500 * sizeof(float)) == 0);
}

TEST_CASE("SvfBank voices are bit-identical to independent Svfs in every mode",
          "[signal][svf][bank][determinism]") {
    constexpr std::size_t kVoices = 16;
    SvfBank<kVoices> bank;
    std::array<Svf, kVoices> scalar;
    for (std::size_t v = 0; v < kVoices; ++v) {
        const auto mode = static_cast<Svf::Mode>(v % 4);
        const float f = 200.0f + 300.0f * static_cast<float>(v);
        const float q = 0.5f + 0.3f * static_cast<float>(v % 5);
        scalar[v].configure(mode, 48000.0f, f, q);
        bank.configure(v, mode, 48000.0f, f, q);
    }
    std::size_t mismatches = 0;
    for (int n = 0; n < 12000; ++n) {
        if (n == 5000) {
            scalar[2].set_mode(Svf::Mode::notch);
            bank.set_mode(2, Svf::Mode::notch);
            scalar[5].set_frequency(9000.0f);
            bank.set_frequency(5, 9000.0f);
            scalar[11].set_resonance(8.0f);
            bank.set_resonance(11, 8.0f);
            scalar[13].reset();
            bank.reset(13);
        }
        std::array<float, kVoices> in{};
        for (std::size_t v = 0; v < kVoices; ++v)
            in[v] = std::sin(0.017f * static_cast<float>(n) * static_cast<float>(v + 1));
        std::array<float, kVoices> out{};
        bank.process(in.data(), out.data());
        for (std::size_t v = 0; v < kVoices; ++v) {
            const float expected = scalar[v].process(in[v]);
            mismatches += std::memcmp(&expected, &out[v], sizeof(float)) != 0;
        }
    }
    CHECK(mismatches == 0);
    const auto c = bank.coefficients(5);
    const auto d = Svf::design(48000.0f, 9000.0f, 0.5f + 0.3f * 0.0f);
    CHECK(c.g == d.g);
}

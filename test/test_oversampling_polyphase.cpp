#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/signal/halfband_iir.hpp>
#include <pulp/signal/oversampling.hpp>

#include "test_oversampling_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

// Polyphase IIR lane: stage designs, alias and image rejection with negative
// controls, passband, float precision, and decay to exact zero on silence.

using Catch::Matchers::WithinAbs;
using pulp::signal::Oversampler;
using pulp::signal::Oversampler64;
using pulp::test::audio::tone_gain_db;
using pulp::test::oversampling::FlushToZeroOff;
using pulp::test::oversampling::injected_tone_rejection_db;
using pulp::test::oversampling::kPi;
using pulp::test::oversampling::settled_tail;

namespace {

using pulp::test::audio::fit_tone;

// The measurement itself, over any x2-or-higher round trip `step(input,
// callback)`: inject a unit tone at `oversampled_frequency` inside the
// callback, let the poles ring down, and report how far the output RMS sits
// below the tone's. `tone` = false injects silence (the analyzer's own floor).
template <typename Step>
double injected_rejection_db(Step&& step, double oversampled_frequency, bool tone = true) {
    constexpr std::size_t frames = 8192;
    constexpr std::size_t settled = 2048;
    std::size_t callback_frame = 0;
    double energy = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double output = step(0.0, [&](double) {
            const double value = tone ? std::sin(2.0 * kPi * oversampled_frequency *
                                                 static_cast<double>(callback_frame))
                                      : 0.0;
            ++callback_frame;
            return value;
        });
        if (i >= settled)
            energy += output * output;
    }
    const double mean_square = energy / static_cast<double>(frames - settled);
    return 10.0 * std::log10(0.5 / (mean_square + 1e-300));
}

double polyphase_rejection_db(Oversampler64::Quality quality, Oversampler64::Factor factor,
                              double frequency, bool tone = true) {
    Oversampler64 oversampler;
    oversampler.set_kind(Oversampler64::Kind::polyphase_iir);
    oversampler.set_quality(quality);
    oversampler.set_factor(factor);
    return injected_rejection_db(
        [&](double input, auto&& callback) { return oversampler.process(input, callback); },
        frequency / static_cast<double>(factor), tone);
}

// The same x2 round trip the oversampler runs, wired from a bare half-band
// stage so a test can hand it a design the oversampler would never choose.
double halfband_x2_rejection_db(const pulp::signal::HalfBandIirDesign& design, double frequency) {
    pulp::signal::HalfBandIirStage64 stage;
    stage.configure(design);
    return injected_rejection_db(
        [&](double input, auto&& callback) {
            double up[2];
            stage.upsample(&input, up, 1);
            up[0] = callback(up[0]);
            up[1] = callback(up[1]);
            double out = 0.0;
            stage.downsample(up, &out, 1);
            return out;
        },
        frequency / 2.0);
}

constexpr std::array<Oversampler64::Factor, 4> kFactors = {
    Oversampler64::Factor::x2, Oversampler64::Factor::x4, Oversampler64::Factor::x8,
    Oversampler64::Factor::x16};

} // namespace

TEST_CASE("Each polyphase quality publishes one stopband figure",
          "[signal][oversampling][polyphase_iir][stopband]") {
    // The figure every document quotes (113.3 dB standard, 146.3 dB pristine)
    // is the worst stage design's equiripple level. Pin both the published
    // function and its derivation from the stage designs, so the header, the
    // Quality comment, the reference page, and the migration note cannot drift
    // from what the filters do.
    for (const auto quality :
         {Oversampler64::Quality::standard, Oversampler64::Quality::pristine}) {
        double worst = -1e300;
        for (int stage = 0; stage < 4; ++stage)
            worst = std::max(worst, pulp::signal::halfband_iir_stopband_db(
                                        Oversampler64::polyphase_stage_design(quality, stage)));
        const double published = Oversampler64::polyphase_stopband_db(quality);
        REQUIRE(published == worst);
        // A design whose figure rounds (toward zero) to anything else at one
        // decimal makes every quoted number wrong.
        const double quoted = quality == Oversampler64::Quality::pristine ? -146.3 : -113.3;
        INFO("quality=" << static_cast<int>(quality) << " published=" << published);
        REQUIRE(published <= quoted);
        REQUIRE(published > quoted - 0.1);
    }
}

TEST_CASE("Polyphase stage designs exist only for the four stages",
          "[signal][oversampling][polyphase_iir]") {
    for (const auto quality :
         {Oversampler64::Quality::standard, Oversampler64::Quality::pristine}) {
        REQUIRE_FALSE(Oversampler64::polyphase_stage_design(quality, -1).valid());
        REQUIRE_FALSE(Oversampler64::polyphase_stage_design(quality, 4).valid());
        for (int stage = 0; stage < 4; ++stage)
            REQUIRE(Oversampler64::polyphase_stage_design(quality, stage).valid());
    }
}

TEST_CASE("Polyphase IIR stage designs meet their analytic floors",
          "[signal][oversampling][polyphase_iir][stopband]") {
    for (const auto quality :
         {Oversampler64::Quality::standard, Oversampler64::Quality::pristine}) {
        const bool pristine = quality == Oversampler64::Quality::pristine;
        for (int stage = 0; stage < 4; ++stage) {
            const auto design = Oversampler64::polyphase_stage_design(quality, stage);
            REQUIRE(design.valid());
            // Every stage keeps the same absolute base band: 0.44 Fs / 0.45 Fs.
            const double rate = static_cast<double>(1 << (stage + 1));
            REQUIRE_THAT(design.passband_edge * rate, WithinAbs(pristine ? 0.45 : 0.44, 1e-12));
            const double stop = pulp::signal::halfband_iir_stopband_db(design);
            INFO("quality=" << pristine << " stage=" << stage << " stopband=" << stop << " dB");
            REQUIRE(stop < (pristine ? -146.0 : -113.3));
        }
    }
}

TEST_CASE("Polyphase IIR rejects callback content folding into the base band at every stage",
          "[signal][oversampling][polyphase_iir][stopband]") {
    // Stage s decimates from 2^(s+1) Fs, and its stopband in base units starts at
    // 2^s - edge, where edge is the base passband (0.44 / 0.45). Content there
    // folds onto the base band through that stage alone, so one window per stage
    // pins every stage independently. Offsets crowd toward the stopband edge,
    // where an equiripple design is no better than anywhere else but where a
    // mis-designed one is weakest.
    constexpr std::array<double, 8> kOffsets = {0.0005, 0.003, 0.01, 0.03, 0.08, 0.15, 0.25, 0.4};
    for (const auto quality :
         {Oversampler64::Quality::standard, Oversampler64::Quality::pristine}) {
        const bool pristine = quality == Oversampler64::Quality::pristine;
        const double edge = pristine ? 0.45 : 0.44;
        const double floor_db = pristine ? 140.0 : 113.0;
        for (const auto factor : kFactors) {
            double worst = 1e9;
            double worst_frequency = 0.0;
            for (int stage = 0; (1 << stage) < static_cast<int>(factor); ++stage) {
                for (const double offset : kOffsets) {
                    if (offset >= edge)
                        continue;
                    const double frequency = static_cast<double>(1 << stage) - edge + offset;
                    const double rejection = polyphase_rejection_db(quality, factor, frequency);
                    if (rejection < worst) {
                        worst = rejection;
                        worst_frequency = frequency;
                    }
                }
            }
            INFO("pristine=" << pristine << " factor=" << static_cast<int>(factor)
                             << " worst rejection=" << worst << " dB at " << worst_frequency
                             << " Fs (floor " << floor_db << ")");
            REQUIRE(worst > floor_db);
        }
    }
}

TEST_CASE("Polyphase IIR rejection gate sits above the analyzer floor and below a weak filter",
          "[signal][oversampling][polyphase_iir][stopband]") {
    // Negative control: identical measurement, nothing injected. The reading
    // must sit far beyond the pristine gate (140 dB) or that gate is grading
    // the analyzer, not the filter.
    const double silent = polyphase_rejection_db(Oversampler64::Quality::pristine,
                                                 Oversampler64::Factor::x2, 0.6, false);
    INFO("analyzer floor reading=" << silent << " dB");
    REQUIRE(silent > 250.0);
    // Planted near-miss through the same measurement: one section short of the
    // pristine stage-0 design (10 instead of 11 at the same 0.225 edge, an
    // equiripple floor near -133 dB) must fail the 140 dB gate somewhere in
    // the window that gate scans. The hand-wired stage is first shown to read
    // what the oversampler reads with the production design, so the near-miss
    // is measured by the same instrument rather than a lookalike.
    {
        constexpr std::array<double, 8> kEdgeOffsets = {0.0005, 0.003, 0.01, 0.03,
                                                        0.08,   0.15,  0.25, 0.4};
        const auto production =
            Oversampler64::polyphase_stage_design(Oversampler64::Quality::pristine, 0);
        const auto near_miss = pulp::signal::design_halfband_iir(10, 0.225);
        double production_worst = 1e9;
        double near_miss_worst = 1e9;
        for (const double offset : kEdgeOffsets) {
            const double frequency = 1.0 - 0.45 + offset;
            const double via_oversampler = polyphase_rejection_db(
                Oversampler64::Quality::pristine, Oversampler64::Factor::x2, frequency);
            const double via_stage = halfband_x2_rejection_db(production, frequency);
            REQUIRE_THAT(via_stage, WithinAbs(via_oversampler, 1e-6));
            production_worst = std::min(production_worst, via_stage);
            near_miss_worst =
                std::min(near_miss_worst, halfband_x2_rejection_db(near_miss, frequency));
        }
        INFO("production worst=" << production_worst << " dB, near-miss worst=" << near_miss_worst
                                 << " dB");
        REQUIRE(production_worst > 140.0);
        REQUIRE(near_miss_worst < 140.0);
        REQUIRE(near_miss_worst > 125.0); // a near miss, not a broken filter
    }
    // Positive control: a passband tone injected in the callback comes straight
    // back, so the same instrument reads ~0 dB rather than a flattering number.
    const double passband =
        polyphase_rejection_db(Oversampler64::Quality::standard, Oversampler64::Factor::x2, 0.2);
    REQUIRE_THAT(passband, WithinAbs(0.0, 0.01));
    // The biquad lane's ~9 dB at the same kind of probe shows the gate fails on
    // a filter that does not reject.
    REQUIRE(injected_tone_rejection_db(Oversampler64::Kind::fir_biquad,
                                       Oversampler64::Quality::standard, Oversampler64::Factor::x2,
                                       0.6) < 30.0);
}

TEST_CASE("Polyphase IIR interpolator rejects base-band images by its design floor",
          "[signal][oversampling][polyphase_iir][stopband]") {
    // A base tone f appears in the oversampled stream at f and at k*Fs +/- f.
    // Project each image by least squares and compare to the tone itself.
    for (const auto quality :
         {Oversampler64::Quality::standard, Oversampler64::Quality::pristine}) {
        const bool pristine = quality == Oversampler64::Quality::pristine;
        for (const auto factor : {Oversampler64::Factor::x2, Oversampler64::Factor::x4}) {
            for (const double frequency : {0.05, 0.2, pristine ? 0.449 : 0.439}) {
                Oversampler64 oversampler;
                oversampler.set_kind(Oversampler64::Kind::polyphase_iir);
                oversampler.set_quality(quality);
                oversampler.set_factor(factor);
                const int f = static_cast<int>(factor);
                std::vector<double> stream;
                stream.reserve(8192u * static_cast<std::size_t>(f));
                for (std::size_t i = 0; i < 8192; ++i)
                    static_cast<void>(oversampler.process(
                        0.5 * std::sin(2.0 * kPi * frequency * static_cast<double>(i)),
                        [&](double sample) {
                            stream.push_back(sample);
                            return sample;
                        }));
                // Fit the tone, subtract it, then fit each image in the residual.
                // A single-tone fit at an image frequency would otherwise pick up
                // the 0 dB tone's finite-window leakage (about -90 dB).
                const auto tail = std::span<const double>(stream).subspan(2048u * f);
                const auto main = fit_tone(tail, frequency / f);
                std::vector<double> residual(tail.begin(), tail.end());
                for (std::size_t n = 0; n < residual.size(); ++n) {
                    const double phase = 2.0 * kPi * frequency / f * static_cast<double>(n);
                    residual[n] -=
                        main.sin_gain * std::sin(phase) + main.cos_gain * std::cos(phase);
                }
                double worst_db = -1e9;
                for (int k = 1; k < f; ++k) {
                    for (const double image : {k - frequency, k + frequency}) {
                        if (image >= 0.5 * f)
                            continue;
                        worst_db = std::max(
                            worst_db, 20.0 * std::log10(fit_tone(residual, image / f).amplitude /
                                                            main.amplitude +
                                                        1e-300));
                    }
                }
                INFO("pristine=" << pristine << " factor=" << f << " f=" << frequency
                                 << " worst image=" << worst_db << " dB");
                REQUIRE(worst_db < (pristine ? -140.0 : -113.0));
            }
        }
    }
}

TEST_CASE("Polyphase IIR passband is flat to its advertised edge",
          "[signal][oversampling][polyphase_iir][passband]") {
    for (const auto quality :
         {Oversampler64::Quality::standard, Oversampler64::Quality::pristine}) {
        const double edge = quality == Oversampler64::Quality::pristine ? 0.45 : 0.44;
        for (const auto factor : kFactors) {
            for (const double frequency : {0.01, 0.1, 0.3, edge - 0.001}) {
                Oversampler64 oversampler;
                oversampler.set_kind(Oversampler64::Kind::polyphase_iir);
                oversampler.set_quality(quality);
                oversampler.set_factor(factor);
                std::vector<double> output(8192);
                for (std::size_t i = 0; i < output.size(); ++i)
                    output[i] = oversampler.process(
                        0.5 * std::sin(2.0 * kPi * frequency * static_cast<double>(i)),
                        [](double sample) { return sample; });
                const double gain = tone_gain_db(settled_tail(output, 2048), frequency, 0.5);
                INFO("edge=" << edge << " factor=" << static_cast<int>(factor) << " f=" << frequency
                             << " gain=" << gain << " dB");
                REQUIRE(std::abs(gain) < 1e-4);
            }
        }
    }
}

TEST_CASE("Polyphase IIR float build holds its stopband against rounding",
          "[signal][oversampling][polyphase_iir][precision]") {
    for (const auto quality : {Oversampler::Quality::standard, Oversampler::Quality::pristine}) {
        Oversampler oversampler;
        oversampler.set_kind(Oversampler::Kind::polyphase_iir);
        oversampler.set_quality(quality);
        oversampler.set_factor(Oversampler::Factor::x2);
        const bool pristine = quality == Oversampler::Quality::pristine;
        const double frequency = pristine ? 0.56 : 0.57; // just past each stopband edge
        std::size_t callback_frame = 0;
        double energy = 0.0;
        constexpr std::size_t frames = 8192;
        for (std::size_t i = 0; i < frames; ++i) {
            const float output = oversampler.process(0.0f, [&](float) {
                const float value =
                    static_cast<float>(std::sin(2.0 * kPi * frequency / 2.0 * callback_frame));
                ++callback_frame;
                return value;
            });
            if (i >= 2048)
                energy += static_cast<double>(output) * output;
        }
        const double rms_db = 10.0 * std::log10(energy / (frames - 2048) / 0.5 + 1e-300);
        INFO("pristine=" << pristine << " float stopband reading=" << rms_db << " dB");
        // Double stage state lets the float build meet the design figure.
        REQUIRE(rms_db < (pristine ? -146.0 : -113.0));
    }
}

namespace {

// Drive a saturated tone, then silence; return the index (into the silence)
// one past the last nonzero output, or -1 if the output never reached zero.
template <typename OS, typename Sample>
long samples_to_exact_zero(typename OS::Kind kind, typename OS::Quality quality,
                           typename OS::Factor factor, std::size_t& subnormal_outputs) {
    OS oversampler;
    oversampler.set_kind(kind);
    oversampler.set_quality(quality);
    oversampler.set_factor(factor);
    std::vector<Sample> block(512);
    auto shaper = [](Sample x) { return std::tanh(Sample{3} * x); };
    for (int b = 0; b < 20; ++b) {
        for (std::size_t i = 0; i < block.size(); ++i)
            block[i] = Sample(0.9) * std::sin(Sample(0.1) * static_cast<Sample>(b * 512 + i));
        oversampler.process_block(block.data(), block.data(), block.size(), shaper);
    }
    long last_nonzero = -1;
    subnormal_outputs = 0;
    constexpr int kSilentBlocks = 128;
    for (int b = 0; b < kSilentBlocks; ++b) {
        std::fill(block.begin(), block.end(), Sample{0});
        oversampler.process_block(block.data(), block.data(), block.size(), shaper);
        for (std::size_t i = 0; i < block.size(); ++i) {
            if (block[i] != Sample{0})
                last_nonzero = static_cast<long>(b * 512 + i);
            if (std::fpclassify(block[i]) == FP_SUBNORMAL)
                ++subnormal_outputs;
        }
    }
    return last_nonzero + 1 < kSilentBlocks * 512 ? last_nonzero + 1 : -1;
}

} // namespace

TEST_CASE("Polyphase IIR output decays to exact zero on silence without flush-to-zero",
          "[signal][oversampling][polyphase_iir][denormal]") {
    // An allpass recurrence fed silence settles into a subnormal limit cycle
    // that never reaches zero: nonzero output forever and, without FTZ, a
    // heavy per-sample cost. The stages flush tiny state at block ends, so the
    // output must hit exact zero within a few thousand samples.
    FlushToZeroOff no_ftz;
    REQUIRE_FALSE(pulp::signal::denormals_are_flushed());
    REQUIRE(std::fpclassify(std::numeric_limits<float>::min() / 4.0f) == FP_SUBNORMAL);
    // Measured: float reaches zero by 1152 (standard) / 1664 (pristine)
    // samples; double, whose flush threshold sits ~600 dB lower, by 10240 /
    // 15744. The bounds leave room without letting a limit cycle pass.
    constexpr long kBound = 4096;
    constexpr long kBound64 = 32768;
    for (const auto quality : {Oversampler::Quality::standard, Oversampler::Quality::pristine}) {
        for (const auto factor :
             {Oversampler::Factor::x2, Oversampler::Factor::x4, Oversampler::Factor::x16}) {
            std::size_t subnormals = 0;
            const long zero_at = samples_to_exact_zero<Oversampler, float>(
                Oversampler::Kind::polyphase_iir, quality, factor, subnormals);
            std::size_t subnormals64 = 0;
            const long zero_at64 = samples_to_exact_zero<Oversampler64, double>(
                Oversampler64::Kind::polyphase_iir, static_cast<Oversampler64::Quality>(quality),
                static_cast<Oversampler64::Factor>(factor), subnormals64);
            INFO("quality=" << static_cast<int>(quality) << " factor=" << static_cast<int>(factor)
                            << " float zero at " << zero_at << " (" << subnormals
                            << " subnormal outputs), double zero at " << zero_at64);
            REQUIRE(zero_at >= 0);
            REQUIRE(zero_at <= kBound);
            REQUIRE(subnormals == 0);
            REQUIRE(zero_at64 >= 0);
            REQUIRE(zero_at64 <= kBound64);
        }
    }
}

TEST_CASE("Float polyphase round trip sits on the float output's own rounding floor",
          "[signal][oversampling][polyphase_iir][precision]") {
    // The float stages keep their state in double, so the only rounding a
    // float round trip adds over the double one is the float samples between
    // stages and at the output: -148 to -152 dB here (x4 crosses one more
    // float stage boundary than x2). Float state measured -132 to -139 dB
    // (pristine, with eleven sections a stage, worst), which is what the
    // -146 dB gate separates from.
    for (const auto quality : {Oversampler::Quality::standard, Oversampler::Quality::pristine}) {
        for (const auto factor : {Oversampler::Factor::x2, Oversampler::Factor::x4}) {
            for (const double frequency : {0.0215, 0.104, 0.208, 0.31}) {
                Oversampler single;
                Oversampler64 dual;
                for (auto* o : {&single}) {
                    o->set_kind(Oversampler::Kind::polyphase_iir);
                    o->set_quality(quality);
                    o->set_factor(factor);
                }
                dual.set_kind(Oversampler64::Kind::polyphase_iir);
                dual.set_quality(static_cast<Oversampler64::Quality>(quality));
                dual.set_factor(static_cast<Oversampler64::Factor>(factor));
                constexpr std::size_t frames = 32768;
                std::vector<float> in(frames), out(frames);
                std::vector<double> in64(frames), out64(frames);
                for (std::size_t n = 0; n < frames; ++n) {
                    in[n] = static_cast<float>(0.9 * std::sin(2.0 * kPi * frequency * n + 0.3));
                    in64[n] = in[n];
                }
                single.process_block(in.data(), out.data(), frames, [](float s) { return s; });
                dual.process_block(in64.data(), out64.data(), frames, [](double s) { return s; });
                double err = 0.0, ref = 0.0;
                for (std::size_t n = 4096; n < frames; ++n) {
                    const double d = static_cast<double>(out[n]) - out64[n];
                    err += d * d;
                    ref += out64[n] * out64[n];
                }
                const double floor_db = 10.0 * std::log10(err / ref);
                INFO("quality=" << static_cast<int>(quality)
                                << " factor=" << static_cast<int>(factor) << " f=" << frequency
                                << " float-vs-double floor=" << floor_db << " dB");
                REQUIRE(floor_db < -146.0);
            }
        }
    }
}

// Multi-character delay — audio-domain acceptance suite.
//
// Every case measures rendered output rather than implementation detail. Shared
// deterministic stimuli and measurements live in support/character_delay_fixture.hpp.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "support/character_delay_fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

using namespace pulp::test::character_delay;

namespace {

// A coherent, exactly band-limited 2947 Hz saw. A naive sampled saw already
// contains aliases, so it cannot distinguish source aliasing from products the
// hysteresis path folds back. Placing every line on an FFT bin makes the
// analysis rectangular-window exact: no leakage skirt or guessed guard band.
constexpr std::size_t kAliasFftSize = 1u << 17;
constexpr std::size_t kAliasFundamentalBin = 8047; // 2946.899 Hz (0.004% from 2947)
constexpr int kAliasInputHarmonics = 7;            // highest line below 0.45 * fs

double band_limited_alias_saw(std::size_t sample) {
    double value = 0.0;
    for (int harmonic = 1; harmonic <= kAliasInputHarmonics; ++harmonic)
        value += std::sin(2.0 * cd::kPi * static_cast<double>(harmonic) *
                          static_cast<double>(kAliasFundamentalBin) * static_cast<double>(sample) /
                          static_cast<double>(kAliasFftSize)) /
                 static_cast<double>(harmonic);
    return value;
}

std::vector<double> hysteresis_alias_spectrum(int oversampling) {
    REQUIRE((oversampling == 1 || oversampling == 4 || oversampling == 8 || oversampling == 16));

    cd::JilesAthertonHysteresis hysteresis;
    hysteresis.prepare(kSr * static_cast<double>(oversampling));
    hysteresis.set_character(cd::kTapeDrive.back(), cd::kTapeBias.back());

    cd::OversampledHysteresis4x four_times;
    cd::OversampledHysteresis8x eight_times;
    if (oversampling == 4) {
        four_times.prepare(kSr);
        four_times.set_character(cd::kTapeDrive.back(), cd::kTapeBias.back());
    }
    if (oversampling == 8) {
        eight_times.prepare(kSr);
        eight_times.set_character(cd::kTapeDrive.back(), cd::kTapeBias.back());
    }

    // The 16x reference extends the shipping topology by one stage, but remains
    // test-local because 8x is the production realization.
    using OversampledHysteresis16x =
        cd::OversampledHysteresis<cd::HysteresisHalfBandOversampler<4>>;
    OversampledHysteresis16x sixteen_times;
    if (oversampling == 16) {
        sixteen_times.prepare(kSr);
        sixteen_times.set_character(cd::kTapeDrive.back(), cd::kTapeBias.back());
    }

    double stimulus_peak = 0.0;
    for (std::size_t sample = 0; sample < 2 * kAliasFftSize / kAliasFundamentalBin + 64; ++sample)
        stimulus_peak = std::max(stimulus_peak, std::abs(band_limited_alias_saw(sample)));
    REQUIRE(stimulus_peak > 0.0);
    constexpr double kInputPeak = 0.25;
    const double stimulus_scale = kInputPeak / stimulus_peak;

    constexpr std::size_t kDiscard = static_cast<std::size_t>(kSr);
    std::vector<std::complex<double>> spectrum(kAliasFftSize);
    for (std::size_t sample = 0; sample < kDiscard + kAliasFftSize; ++sample) {
        const double input = stimulus_scale * band_limited_alias_saw(sample);
        double output = 0.0;
        switch (oversampling) {
        case 1:
            output = hysteresis.process(input);
            break;
        case 4:
            output = four_times.process(input);
            break;
        case 8:
            output = eight_times.process(input);
            break;
        case 16:
            output = sixteen_times.process(input);
            break;
        default:
            break;
        }
        if (sample >= kDiscard)
            spectrum[sample - kDiscard] = output;
    }

    pulp::signal::FftT<double> fft(static_cast<int>(kAliasFftSize));
    fft.forward(spectrum.data());
    std::vector<double> magnitude(kAliasFftSize / 2 + 1);
    for (std::size_t bin = 0; bin < magnitude.size(); ++bin)
        magnitude[bin] = std::abs(spectrum[bin]);
    return magnitude;
}

double largest_alias_reference_harmonic(const std::vector<double>& reference) {
    double largest = 0.0;
    for (int harmonic = 1; harmonic <= kAliasInputHarmonics; ++harmonic)
        largest =
            std::max(largest, reference[static_cast<std::size_t>(harmonic) * kAliasFundamentalBin]);
    return largest;
}

// Excess over a high-rate reference, relative to its largest legitimate
// harmonic. Only the seven exact stimulus bins (plus two numerical guard bins)
// are excluded. The scan is band-qualified to 0.45 * fs because no finite
// decimator can reject content arbitrarily close to Nyquist.
double hysteresis_alias_excess_dbc(const std::vector<double>& dut,
                                   const std::vector<double>& reference) {
    REQUIRE(dut.size() == reference.size());
    const double largest_harmonic = largest_alias_reference_harmonic(reference);
    REQUIRE(largest_harmonic > 0.0);

    std::vector<bool> stimulus_line(reference.size(), false);
    for (int harmonic = 1; harmonic <= kAliasInputHarmonics; ++harmonic) {
        const std::size_t centre = static_cast<std::size_t>(harmonic) * kAliasFundamentalBin;
        for (std::size_t bin = centre - 2; bin <= centre + 2 && bin < stimulus_line.size(); ++bin)
            stimulus_line[bin] = true;
    }

    const double bin_hz = kSr / static_cast<double>(kAliasFftSize);
    const auto low = static_cast<std::size_t>(30.0 / bin_hz);
    const auto high = static_cast<std::size_t>(0.45 * kSr / bin_hz);
    double worst_excess = 0.0;
    for (std::size_t bin = low; bin <= high && bin < dut.size(); ++bin) {
        if (stimulus_line[bin])
            continue;
        worst_excess = std::max(worst_excess, dut[bin] - reference[bin]);
    }
    return 20.0 * std::log10(std::max(worst_excess, 1.0e-300) / largest_harmonic);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 15 — Jiles-Atherton hysteresis
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the hysteresis solver converges inside its iteration cap",
          "[character-delay][hysteresis][slow]") {
    // 4x the saturation onset: the Langevin function saturates around an
    // argument of 3, and the argument is field/shape, so the onset field is
    // 3 x shape and the drive level is four times that.
    for (double drive : {cd::kTapeDrive.front(), cd::kTapeDrive[2], cd::kTapeDrive.back()}) {
        cd::JilesAthertonHysteresis hysteresis;
        hysteresis.prepare(kSr * 4.0);
        hysteresis.set_character(drive, cd::kTapeBias[1]);
        hysteresis.clear_solver_counters();

        const int n = static_cast<int>(kSr * 4.0 * 10.0);
        for (int i = 0; i < n; ++i)
            hysteresis.process(0.75 * std::sin(2.0 * cd::kPi * 1000.0 * i / (kSr * 4.0)));

        INFO("drive " << drive << " capped " << hysteresis.capped_steps());
        CHECK(hysteresis.capped_steps() == 0);
    }

    // Bounded output at maximum drive, not just convergence.
    cd::JilesAthertonHysteresis hot;
    hot.prepare(kSr * 4.0);
    hot.set_character(cd::kTapeDrive.back(), cd::kTapeBias.back());
    const int n = static_cast<int>(kSr * 4.0 * 10.0);
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max(worst, std::abs(hot.process(
                                    0.75 * std::sin(2.0 * cd::kPi * 1000.0 * i / (kSr * 4.0)))));
    INFO("max-drive peak " << worst);
    CHECK(worst < 1.5);
}

TEST_CASE("hysteresis has loop area, unlike a waveshaper",
          "[character-delay][hysteresis]") {
    cd::JilesAthertonHysteresis hysteresis;
    hysteresis.prepare(kSr);
    hysteresis.set_character(cd::kTapeDrive[2], cd::kTapeBias[1]);

    double area = 0.0;
    double previous_field = 0.0;
    double previous_magnetization = 0.0;
    const int n = static_cast<int>(kSr);
    for (int i = 0; i < n; ++i) {
        const double field = 0.6 * std::sin(2.0 * cd::kPi * 2.0 * i / kSr);
        const double magnetization = hysteresis.process(field);
        area += 0.5 * (magnetization + previous_magnetization) * (field - previous_field);
        previous_field = field;
        previous_magnetization = magnetization;
    }
    INFO("loop area " << area);
    CHECK(std::abs(area) > 1e-3);
}

TEST_CASE("hysteresis distortion grows with drive and silence clears it",
          "[character-delay][hysteresis]") {
    auto distortion = [](double drive) {
        cd::JilesAthertonHysteresis hysteresis;
        hysteresis.prepare(kSr);
        hysteresis.set_character(drive, cd::kTapeBias[1]);
        const int n = static_cast<int>(kSr);
        std::vector<float> out(static_cast<std::size_t>(n), 0.0f);
        for (int i = 0; i < n; ++i)
            out[static_cast<std::size_t>(i)] = static_cast<float>(
                hysteresis.process(0.4 * std::sin(2.0 * cd::kPi * 500.0 * i / kSr)));

        const double fundamental = magnitude_at(out, n / 4, n, 500.0);
        double harmonics = 0.0;
        for (int h = 2; h <= 6; ++h) {
            const double m = magnitude_at(out, n / 4, n, 500.0 * h);
            harmonics += m * m;
        }
        return std::sqrt(harmonics) / std::max(fundamental, 1e-12);
    };

    const double soft = distortion(cd::kTapeDrive.front());
    const double hard = distortion(cd::kTapeDrive.back());
    INFO("THD soft " << soft << " hard " << hard);
    CHECK(hard > soft);

    cd::JilesAthertonHysteresis hysteresis;
    hysteresis.prepare(kSr);
    hysteresis.set_character(cd::kTapeDrive[2], cd::kTapeBias[2]);
    for (int i = 0; i < 4800; ++i)
        hysteresis.process(0.5 * std::sin(2.0 * cd::kPi * 100.0 * i / kSr));
    double last = 1.0;
    for (int i = 0; i < 4800; ++i) last = hysteresis.process(0.0);
    INFO("magnetization after silence " << last);
    CHECK(last == 0.0);
}

TEST_CASE("hysteresis oversampler depth owns factor, phase count, and latency",
          "[character-delay][hysteresis]") {
    using FourTimes = cd::HysteresisHalfBandOversampler<2>;
    using EightTimes = cd::HysteresisHalfBandOversampler<3>;
    static_assert(FourTimes::oversampling_factor() == 4);
    static_assert(EightTimes::oversampling_factor() == 8);

    const auto taps = static_cast<int>(cd::kHysteresisHalfBandTaps);
    CHECK(FourTimes::latency_samples() == (taps - 1) / 2 + (taps - 1) / 4);
    CHECK(EightTimes::latency_samples() ==
          (taps - 1) / 2 + (taps - 1) / 4 + (taps - 1) / 8);

    FourTimes four;
    EightTimes eight;
    four.prepare();
    eight.prepare();
    int four_phases = 0;
    int eight_phases = 0;
    (void)four.process(0.0, [&](double value) {
        ++four_phases;
        return value;
    });
    (void)eight.process(0.0, [&](double value) {
        ++eight_phases;
        return value;
    });
    CHECK(four_phases == FourTimes::oversampling_factor());
    CHECK(eight_phases == EightTimes::oversampling_factor());
}

TEST_CASE("hysteresis oversampler presents phases in chronological order",
          "[character-delay][hysteresis]") {
    // The callback owns time-dependent magnetic state, so the cascade is only
    // correct if it hands over the oversampled phases in time order. C++ leaves
    // function-argument evaluation order unspecified, which means the ordering
    // is a property the cascade must sequence deliberately rather than one the
    // language supplies — exactly the invariant a recursive rewrite can lose
    // while every factor, latency and phase-count check stays green.
    //
    // A linear-phase symmetric FIR reproduces a ramp exactly, delayed and
    // scaled, so the recorded phase sequence of a rising ramp must itself rise
    // monotonically at the oversampled rate. Any transposed pair is a step
    // backwards. Checked on the shipping 8x depth, whose three stages exercise
    // both the recursion and its leaf.
    cd::HysteresisHalfBandOversampler<3> eight;
    eight.prepare();

    constexpr int kHostSamples = 256;
    constexpr int kSettleHostSamples = 64;  // past the cascade's group delay
    const auto factor = static_cast<std::size_t>(
        cd::HysteresisHalfBandOversampler<3>::oversampling_factor());

    std::vector<double> phases;
    phases.reserve(static_cast<std::size_t>(kHostSamples) * factor);
    for (int sample = 0; sample < kHostSamples; ++sample) {
        (void)eight.process(static_cast<double>(sample), [&](double value) {
            phases.push_back(value);
            return value;
        });
    }
    REQUIRE(phases.size() == static_cast<std::size_t>(kHostSamples) * factor);

    const std::size_t settled = static_cast<std::size_t>(kSettleHostSamples) * factor;
    double smallest_step = phases[settled + 1] - phases[settled];
    for (std::size_t i = settled + 1; i < phases.size(); ++i)
        smallest_step = std::min(smallest_step, phases[i] - phases[i - 1]);
    CHECK(smallest_step > 0.0);
}

TEST_CASE("physical tape feedback calibration is continuous and releases above unity",
          "[character-delay][feedback][tape]") {
    static_assert(noexcept(cd::physical_tape_effective_feedback(0.0, 0.0)));

    for (std::size_t knot = 0; knot < cd::kTapeAxis.size(); ++knot) {
        const double age = cd::kTapeAxis[knot];
        const double compensation = cd::kTapePhysicalFeedbackCompensation[knot];
        CAPTURE(age, compensation);
        CHECK(cd::physical_tape_effective_feedback(0.0, age) == 0.0);
        CHECK(cd::physical_tape_effective_feedback(0.5, age) ==
              Catch::Approx(0.5 * compensation));
        CHECK(cd::physical_tape_effective_feedback(1.0, age) ==
              Catch::Approx(compensation));
        CHECK(cd::physical_tape_effective_feedback(1.05, age) ==
              Catch::Approx(1.05 * (compensation + 0.5 * (1.0 - compensation))));
        CHECK(cd::physical_tape_effective_feedback(1.1, age) == Catch::Approx(1.1));
    }

    const double midpoint_age = 0.5;
    const double midpoint_compensation = cd::interpolate_knots(
        cd::kTapeAxis, cd::kTapePhysicalFeedbackCompensation, midpoint_age);
    CHECK(cd::physical_tape_effective_feedback(0.8, midpoint_age) ==
          Catch::Approx(0.8 * midpoint_compensation));

    constexpr double epsilon = 1e-9;
    const double below =
        cd::physical_tape_effective_feedback(1.0 - epsilon, midpoint_age);
    const double at_unity =
        cd::physical_tape_effective_feedback(1.0, midpoint_age);
    const double above =
        cd::physical_tape_effective_feedback(1.0 + epsilon, midpoint_age);
    CHECK(below < at_unity);
    CHECK(above > at_unity);
    CHECK(at_unity - below < 1e-8);
    CHECK(above - at_unity < 1e-8);
}

TEST_CASE("physical hysteresis oversampling suppresses max-drive aliasing",
          "[character-delay][hysteresis][aliasing][slow]") {
    const auto reference = hysteresis_alias_spectrum(16);
    const double one_times = hysteresis_alias_excess_dbc(hysteresis_alias_spectrum(1), reference);
    const double four_times = hysteresis_alias_excess_dbc(hysteresis_alias_spectrum(4), reference);
    const double eight_times = hysteresis_alias_excess_dbc(hysteresis_alias_spectrum(8), reference);
    INFO("1x=" << one_times << " dBc, 4x=" << four_times << " dBc, shipping 8x=" << eight_times
               << " dBc");

    // The shipped path must clear the original numeric contract. The prior 4x
    // path is retained as a must-fail control, and every doubling must improve,
    // so a disconnected oversampler cannot satisfy this test accidentally.
    CHECK(eight_times <= -60.0);
    CHECK(four_times > -60.0);
    CHECK(four_times < one_times);
    CHECK(eight_times < four_times);

    // Cheap calibration of the metric itself. Identity must collapse, while a
    // synthetic off-grid component exactly 40 dB below the largest harmonic
    // must be recovered as -40 dBc by the same subtraction and normalization.
    CHECK(hysteresis_alias_excess_dbc(reference, reference) < -200.0);
    auto injected = reference;
    const double bin_hz = kSr / static_cast<double>(kAliasFftSize);
    const auto injected_bin = static_cast<std::size_t>(1000.0 / bin_hz);
    injected[injected_bin] += 0.01 * largest_alias_reference_harmonic(reference);
    CHECK(hysteresis_alias_excess_dbc(injected, reference) == Catch::Approx(-40.0).margin(0.01));
}

TEST_CASE("the hysteresis oversampler preserves linear passband gain",
          "[character-delay][hysteresis]") {
    cd::HysteresisOversampler8x wrapper;
    wrapper.prepare();
    double input_energy = 0.0;
    double output_energy = 0.0;
    for (int sample = 0; sample < static_cast<int>(kSr); ++sample) {
        const double input = 0.1 * std::sin(2.0 * cd::kPi * 1000.0 * sample / kSr);
        const double output = wrapper.process(input, [](double value) { return value; });
        if (sample >= static_cast<int>(0.25 * kSr)) {
            input_energy += input * input;
            output_energy += output * output;
        }
    }
    const double gain = std::sqrt(output_energy / input_energy);
    INFO("8x linear passband gain " << gain);
    CHECK(gain == Catch::Approx(1.0).margin(0.01));

    auto modeled_gain = [](int factor) {
        cd::JilesAthertonHysteresis model;
        model.prepare(kSr * factor);
        model.set_character(cd::kTapeDrive.back(), cd::kTapeBias.back());
        cd::HalfBandOversampler4x four;
        cd::HysteresisOversampler8x eight;
        four.prepare();
        eight.prepare();
        double in_energy = 0.0;
        double out_energy = 0.0;
        for (int sample = 0; sample < static_cast<int>(kSr); ++sample) {
            const double input = 0.02 * std::sin(2.0 * cd::kPi * 1000.0 * sample / kSr);
            const double output = factor == 4
                ? four.process(input, [&](double value) { return model.process(value); })
                : eight.process(input, [&](double value) { return model.process(value); });
            if (sample >= static_cast<int>(0.25 * kSr)) {
                in_energy += input * input;
                out_energy += output * output;
            }
        }
        return std::sqrt(out_energy / in_energy);
    };
    const double gain4 = modeled_gain(4);
    const double gain8 = modeled_gain(8);
    INFO("modeled small-signal gain 4x=" << gain4 << " 8x=" << gain8);
    CHECK(gain8 / gain4 == Catch::Approx(1.0).margin(0.1));
}

// ═══════════════════════════════════════════════════════════════════════════
// 16 — Wallace loss filter
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the loss cascade realizes the modeled response",
          "[character-delay][tape][loss]") {
    // The stage is a cascade: a fitted IIR ladder carries the smooth
    // spacing/thickness tilt and a minimum-phase FIR carries the gap null. This
    // measures the COMBINED response against the analytic model, which is the
    // only comparison that means anything to a listener.
    cd::TapeLossDesign design;
    design.prepare(kSr, 7.5);
    const std::size_t taps = cd::tape_gap_fir_taps(kSr);

    auto worst_error = [&](double ips, double spacing_um) {
        cd::TapeLossGeometry geometry;
        geometry.speed_ips = ips;
        geometry.spacing_m = spacing_um * 1e-6;

        const auto gap = cd::design_tape_gap_fir(kSr, taps, geometry);
        const auto parameters = design.shapes().parameters_for(geometry);

        double worst = 0.0;
        for (int k = 0; k < 10; ++k) {
            const double f = 20.0 * std::pow(0.45 * kSr / 20.0, k / 9.0);
            std::complex<double> sum{0.0, 0.0};
            for (std::size_t i = 0; i < taps; ++i)
                sum += gap[i] * std::exp(std::complex<double>(0.0, -2.0 * cd::kPi * f * i / kSr));
            const double realized = 20.0 * std::log10(std::max(std::abs(sum), 1e-12)) +
                                    cd::tape_loss_iir_magnitude_db(parameters, f);
            const double target = 20.0 * std::log10(std::max(
                                             cd::tape_loss_magnitude_floored(f, geometry), 1e-12));
            worst = std::max(worst, std::abs(std::max(realized, cd::kTapeLossFloorDb) -
                                             std::max(target, cd::kTapeLossFloorDb)));
        }
        return worst;
    };

    // At and above 3.75 ips, within a decibel across the band — at every
    // spacing on the age axis, not just the nominal one.
    for (double ips : {3.75, 7.5, 15.0, 30.0}) {
        for (double spacing : cd::kAgeSpacingUm) {
            const double error = worst_error(ips, spacing);
            INFO(ips << " ips at " << spacing << " um: worst error " << error << " dB");
            CHECK(error < 1.0);
        }
    }

    // The slowest speed at maximum wear is the hardest corner in the model —
    // the analytic −3 dB point drops near 100 Hz there. Held to 2 dB.
    const double worn = worst_error(1.875, cd::kAgeSpacingUm.back());
    INFO("1.875 ips worn worst error " << worn << " dB");
    CHECK(worn < 2.0);
}

TEST_CASE("the loss cascade is exact at every age, not just at fitted points",
          "[character-delay][tape][loss]") {
    // The age axis is a pure frequency SCALING of a fitted dimensionless shape,
    // so there are no knots to fall between. This sweeps age continuously and
    // asserts the accuracy never degrades — the case that would catch a
    // regression back to fitting-and-interpolating, which measured inside 1 dB
    // at its knots and 3-4 dB between them.
    cd::TapeLossDesign design;
    design.prepare(kSr, 7.5);

    for (double age = 0.0; age <= 1.0; age += 0.03125) {
        const auto geometry = design.geometry_at(age);
        const auto parameters = design.parameters_at(age);

        double worst = 0.0;
        for (int k = 0; k < 12; ++k) {
            const double f = 20.0 * std::pow(0.45 * kSr / 20.0, k / 11.0);
            const double realized = cd::tape_loss_iir_magnitude_db(parameters, f);
            const double target = 20.0 * std::log10(std::max(
                                             cd::tape_loss_smooth_magnitude(f, geometry), 1e-12));
            worst = std::max(worst, std::abs(std::max(realized, cd::kTapeLossFloorDb) -
                                             std::max(target, cd::kTapeLossFloorDb)));
        }
        INFO("age " << age << " worst error " << worst << " dB");
        CHECK(worst < 1.0);
    }

    // Scaling is monotone in age: more wear is never brighter.
    double previous = 1e12;
    for (double age = 0.0; age <= 1.0; age += 0.1) {
        const auto parameters = design.parameters_at(age);
        const double at_5k = cd::tape_loss_iir_magnitude_db(parameters, 5000.0);
        INFO("age " << age << " response at 5 kHz " << at_5k << " dB");
        CHECK(at_5k <= previous + 1e-9);
        previous = at_5k;
    }
}

TEST_CASE("the shipped loss shapes reproduce a fresh derivation",
          "[character-delay][tape][loss][slow]") {
    // The shapes in tables.hpp were derived offline by the fitter that still
    // lives in tape_loss.hpp. This re-runs that derivation and checks the
    // shipped values give the same RESPONSE — comparing responses rather than
    // parameters because a minimax fit can reach the same curve through
    // different parameter sets, and it is the curve that is the contract.
    const auto fitted = cd::fit_tape_loss_shapes();
    const auto shipped = cd::TapeLossShapes::tabulated();

    auto magnitude = [](const auto& shape, double x) {
        double db = 0.0;
        for (double corner : shape.pole_x) {
            const double r = x / corner;
            db += -10.0 * std::log10(1.0 + r * r);
        }
        for (std::size_t i = 0; i < shape.shelf_x.size(); ++i) {
            const double g = std::pow(10.0, shape.shelf_db[i] / 20.0);
            const double r = x / shape.shelf_x[i];
            db += 10.0 * std::log10((1.0 + g * g * r * r) / (1.0 + r * r));
        }
        return db;
    };

    double spacing_shipped = 0.0;
    double spacing_fitted = 0.0;
    double thickness_shipped = 0.0;
    for (int k = 0; k < 40; ++k) {
        const double x = cd::kLossShapeMinX *
                         std::pow(cd::kLossShapeMaxX / cd::kLossShapeMinX, k / 39.0);
        const auto clamp_db = [](double v) { return std::max(v, cd::kTapeLossFloorDb); };

        spacing_shipped = std::max(spacing_shipped,
                                   std::abs(clamp_db(magnitude(shipped.spacing, x)) -
                                            clamp_db(cd::spacing_shape_db(x))));
        spacing_fitted = std::max(spacing_fitted,
                                  std::abs(clamp_db(magnitude(fitted.spacing, x)) -
                                           clamp_db(cd::spacing_shape_db(x))));
        thickness_shipped = std::max(thickness_shipped,
                                     std::abs(clamp_db(magnitude(shipped.thickness, x)) -
                                              clamp_db(cd::thickness_shape_db(x))));
    }

    INFO("shipped spacing " << spacing_shipped << " dB, fresh fit " << spacing_fitted
                            << " dB, shipped thickness " << thickness_shipped << " dB");
    CHECK(spacing_shipped < 0.5);
    CHECK(thickness_shipped < 0.3);
    // The shipped table must be at least as good as what the fitter produces
    // now, with a little slack for the search's own run-to-run spread.
    CHECK(spacing_shipped < spacing_fitted + 0.25);
}

TEST_CASE("the gap-loss null lands where the physics predicts",
          "[character-delay][tape][loss]") {
    cd::TapeLossGeometry geometry;
    geometry.speed_ips = 1.875;
    geometry.spacing_m = 5e-6;
    const double predicted = geometry.speed_ips * 0.0254 / geometry.gap_m;

    double best_hz = 0.0;
    double best = 1e30;
    for (double f = 1000.0; f < 0.5 * kSr; f += 5.0) {
        const double m = cd::tape_loss_magnitude(f, geometry);
        if (m < best) {
            best = m;
            best_hz = f;
        }
    }
    INFO("predicted " << predicted << " Hz, measured " << best_hz << " Hz");
    CHECK(std::abs(best_hz - predicted) < 0.01 * predicted);
}

TEST_CASE("a tape speed change crossfades without a discontinuity",
          "[character-delay][tape][loss]") {
    Engine delay;
    configure(delay, Character::tape, 300.0, 0.4, 0.5, TapeTier::physical);
    settle(delay, 1.5);

    auto before = sine_both(static_cast<int>(kSr * 0.5), 400.0, 0.4f);
    render(delay, before);
    const double baseline = max_step(before.left, 1000, static_cast<int>(before.left.size()));

    delay.set_tape_speed_ips(15.0f);
    auto after = sine_both(static_cast<int>(kSr * 0.5), 400.0, 0.4f);
    render(delay, after);

    INFO("baseline step " << baseline << " during crossfade "
                          << max_step(after.left, 0, static_cast<int>(0.05 * kSr)));
    CHECK(all_finite(after.left));
    CHECK(max_step(after.left, 0, static_cast<int>(0.05 * kSr)) <= 2.0 * baseline);
}

TEST_CASE("configuring the tape speed before the sample rate is safe",
          "[character-delay][catalog][tape][lifecycle]") {
    // The catalog node's prepare() sets character, tier and tape speed and only
    // then the sample rate — so a speed change arrives while the physical
    // tier's FIR banks and working buffer do not yet exist. Walking them there
    // is an out-of-bounds write, and it only triggers for nodes constructed at
    // a non-default speed, which is exactly the kind of thing that ships.
    Engine delay;
    delay.set_character(Character::tape);
    delay.set_tape_tier(TapeTier::physical);
    delay.set_tape_speed_ips(15.0f);
    delay.set_sample_rate(kSr);
    delay.set_time_ms(200.0f);
    delay.set_feedback(0.4f);
    delay.set_character_amount(0.5f);
    delay.reset();

    auto buffers = sine_both(static_cast<int>(kSr * 0.5), 500.0, 0.4f);
    render(delay, buffers);
    CHECK(all_finite(buffers.left));
    CHECK(rms(buffers.left, 0, static_cast<int>(buffers.left.size())) > 0.0);
    // The speed the caller asked for is the speed the banks were designed at.
    CHECK(delay.tape_gap_coefficients(0).size() == cd::tape_gap_fir_taps(kSr));
}

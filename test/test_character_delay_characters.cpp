// Multi-character delay — audio-domain acceptance suite.
//
// Every case measures rendered output rather than implementation detail. Shared
// deterministic stimuli and measurements live in support/character_delay_fixture.hpp.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/character_delay/vintage.hpp>

#include "support/character_delay_fixture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

using namespace pulp::test::character_delay;

namespace {

double detrended_phase_rms(const std::vector<double>& phase) {
    if (phase.size() < 2) return 0.0;
    const double n = static_cast<double>(phase.size());
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    for (std::size_t i = 0; i < phase.size(); ++i) {
        const double x = static_cast<double>(i);
        const double y = phase[i];
        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
    }
    const double denominator = n * sum_xx - sum_x * sum_x;
    const double slope = denominator != 0.0 ? (n * sum_xy - sum_x * sum_y) / denominator : 0.0;
    const double intercept = (sum_y - slope * sum_x) / n;
    double energy = 0.0;
    for (std::size_t i = 0; i < phase.size(); ++i) {
        const double residual = phase[i] - (intercept + slope * static_cast<double>(i));
        energy += residual * residual;
    }
    return std::sqrt(energy / n);
}

double vintage_fractional_phase_rms(double carrier_hz) {
    cd::VintageChannel vintage;
    vintage.prepare(kSr);
    vintage.set_seed(0x12345678u);
    vintage.update(0.0);
    vintage.reset();

    constexpr int kDuration = static_cast<int>(5.0 * kSr);
    constexpr int kMeasureFrom = static_cast<int>(2.0 * kSr);
    constexpr double kDelaySeconds = 0.137123;
    constexpr double kAmplitude = 0.3;
    std::array<double, 3> energy{};
    std::array<std::size_t, 3> count{};
    for (int i = 0; i < kDuration; ++i) {
        const double input =
            kAmplitude * std::sin(2.0 * cd::kPi * carrier_hz * static_cast<double>(i) / kSr);
        const double output = vintage.process(input, kDelaySeconds);
        if (i < kMeasureFrom) continue;
        const auto phase_class = static_cast<std::size_t>(i % 3);
        energy[phase_class] += output * output;
        ++count[phase_class];
    }

    std::array<double, 3> class_rms{};
    for (std::size_t i = 0; i < class_rms.size(); ++i)
        class_rms[i] = std::sqrt(energy[i] / static_cast<double>(count[i]));
    const auto [lo, hi] = std::minmax_element(class_rms.begin(), class_rms.end());
    return (*hi - *lo) / std::max(*hi, 1e-12);
}

double measured_wet_tone_gain(Character character, double time_ms, double amount,
                              double hz) {
    Engine delay;
    configure(delay, character, time_ms, 0.0, amount);
    settle(delay, 4.0 * slew_seconds(character) + 0.5);

    const int n = static_cast<int>(kSr * 1.5);
    constexpr float kAmplitude = 0.05f;
    auto buffers = sine_both(n, hz, kAmplitude);
    render(delay, buffers);
    // BBD clock wander spreads a steady carrier into close sidebands. RMS
    // measures the complete wet tone instead of mistaking that intentional
    // modulation for filter loss by looking at one FFT bin.
    return rms(buffers.left, static_cast<int>(kSr), n) * std::sqrt(2.0) / kAmplitude;
}

struct ResponseCrossingMeasurement {
    double reference_hz = 0.0;
    double reference_gain = 0.0;
    std::vector<std::pair<double, double>> probes;
    double crossing_hz = 0.0;
};

template <typename GainAt>
ResponseCrossingMeasurement measure_minus_3db_crossing(double expected_hz,
                                                       GainAt&& gain_at,
                                                       double high_factor = 1.8) {
    ResponseCrossingMeasurement result;
    result.reference_hz = std::max(80.0, expected_hz * 0.05);
    result.reference_gain = gain_at(result.reference_hz);
    if (!(result.reference_gain > 1e-6)) return result;

    const double threshold = result.reference_gain / std::sqrt(2.0);
    constexpr int kPoints = 17;
    const double low = std::max(100.0, expected_hz * 0.35);
    const double high = std::min(0.45 * kSr, expected_hz * high_factor);
    double previous_hz = low;
    double previous_gain = gain_at(previous_hz);
    result.probes.emplace_back(previous_hz, previous_gain);
    for (int point = 1; point < kPoints; ++point) {
        const double fraction = static_cast<double>(point) / (kPoints - 1);
        const double hz = low * std::pow(high / low, fraction);
        const double gain = gain_at(hz);
        result.probes.emplace_back(hz, gain);
        if (gain <= threshold && previous_gain > threshold) {
            const double previous_db =
                20.0 * std::log10(previous_gain / result.reference_gain);
            const double gain_db =
                20.0 * std::log10(gain / result.reference_gain);
            const double mix =
                (-3.01029995664 - previous_db) / (gain_db - previous_db);
            result.crossing_hz =
                std::exp(std::log(previous_hz) +
                         std::clamp(mix, 0.0, 1.0) *
                             (std::log(hz) - std::log(previous_hz)));
            return result;
        }
        previous_hz = hz;
        previous_gain = gain;
    }
    return result;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 5 — BBD bandwidth law
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("BBD bandwidth follows the clock rate", "[character-delay][bbd]") {
    for (double time_ms : {50.0, 500.0}) {
        Engine delay;
        configure(delay, Character::bbd, time_ms, 0.0, 0.5);
        settle(delay, 4.0 * slew_seconds(Character::bbd) + 0.5);

        // Expected value comes from the shipped table, not from a restated
        // number: the law is N/t/3 clamped, whatever N and the clamps are.
        const double stages = static_cast<double>(delay.bbd_stages());
        const double clock = stages / (time_ms * 0.001);
        const double expected = std::clamp(clock / cd::kBbdBandwidthDivisor,
                                           cd::kBbdBandwidthMinHz, cd::kBbdBandwidthMaxHz);

        INFO("time " << time_ms << " ms, stages " << stages << ", reported "
                     << delay.bbd_bandwidth_hz() << ", expected " << expected);
        CHECK(std::abs(delay.bbd_bandwidth_hz() - expected) < 0.15 * expected);
    }

    // And the relationship holds automatically as the time slews between them:
    // short is bright, long is dark, with no curve drawn anywhere.
    Engine delay;
    configure(delay, Character::bbd, 50.0, 0.0, 0.5);
    settle(delay, 1.0);
    const double bright = delay.bbd_bandwidth_hz();
    delay.set_time_ms(500.0f);
    settle(delay, 2.0);
    const double dark = delay.bbd_bandwidth_hz();
    INFO("bright " << bright << " dark " << dark);
    CHECK(dark < bright * 0.5);
}

TEST_CASE("BBD wet audio follows the clock-derived bandwidth",
          "[character-delay][bbd][slow]") {
    for (double time_ms : {50.0, 500.0}) {
        Engine law;
        configure(law, Character::bbd, time_ms, 0.0, 0.5);
        settle(law, 4.0 * slew_seconds(Character::bbd) + 0.5);
        const double expected = law.bbd_bandwidth_hz();
        const auto measured = measure_minus_3db_crossing(
            expected, [=](double hz) {
                return measured_wet_tone_gain(Character::bbd, time_ms, 0.5, hz);
            });
        INFO("time " << time_ms << " ms, measured wet crossing "
                     << measured.crossing_hz << " Hz, clock-derived target "
                     << expected << " Hz");
        REQUIRE(measured.crossing_hz > 0.0);
        CHECK(std::abs(measured.crossing_hz - expected) <= 0.15 * expected);
    }
}

TEST_CASE("BBD wet bandwidth tracks the clock throughout a time slew",
          "[character-delay][bbd][slow]") {
    constexpr std::size_t kToneCount = 25;
    constexpr std::size_t kSnapshotCount = 3;
    constexpr double kDurationS = 10.0;
    constexpr double kWindowS = 0.1;
    const std::array<double, kSnapshotCount> snapshot_seconds{2.0, 5.0, 8.0};

    std::array<double, kToneCount> frequencies{};
    std::array<Engine, kToneCount> delays{};
    for (std::size_t tone = 0; tone < kToneCount; ++tone) {
        const double fraction = static_cast<double>(tone) /
                                static_cast<double>(kToneCount - 1);
        frequencies[tone] = 100.0 * std::pow(120.0, fraction);
        configure(delays[tone], Character::bbd, 50.0, 0.0, 0.5);
        settle(delays[tone], 1.0);
    }

    std::array<std::array<double, kToneCount>, kSnapshotCount> energy{};
    std::array<int, kSnapshotCount> counts{};
    std::array<double, kSnapshotCount> targets{};
    const int total_samples = static_cast<int>(kDurationS * kSr);
    for (int sample = 0; sample < total_samples; ++sample) {
        const double elapsed = static_cast<double>(sample) / kSr;
        const float time_ms = static_cast<float>(50.0 + 450.0 * elapsed / kDurationS);
        for (std::size_t tone = 0; tone < kToneCount; ++tone) {
            delays[tone].set_time_ms(time_ms);
            float left = static_cast<float>(
                0.05 * std::sin(2.0 * cd::kPi * frequencies[tone] * elapsed));
            float right = left;
            delays[tone].process(&left, &right, 1);
            for (std::size_t snapshot = 0; snapshot < kSnapshotCount; ++snapshot) {
                const double end = snapshot_seconds[snapshot];
                if (elapsed >= end - kWindowS && elapsed < end)
                    energy[snapshot][tone] += static_cast<double>(left) * left;
            }
        }
        for (std::size_t snapshot = 0; snapshot < kSnapshotCount; ++snapshot) {
            const double end = snapshot_seconds[snapshot];
            if (elapsed >= end - kWindowS && elapsed < end)
                ++counts[snapshot];
            if (sample == static_cast<int>(end * kSr) - 1)
                targets[snapshot] = delays[0].bbd_bandwidth_hz();
        }
    }

    std::array<double, kSnapshotCount> crossings{};
    for (std::size_t snapshot = 0; snapshot < kSnapshotCount; ++snapshot) {
        REQUIRE(counts[snapshot] > 0);
        std::array<double, kToneCount> gains{};
        for (std::size_t tone = 0; tone < kToneCount; ++tone)
            gains[tone] = std::sqrt(energy[snapshot][tone] / counts[snapshot]);
        const double threshold = gains.front() / std::sqrt(2.0);
        REQUIRE(gains.front() > 1e-6);
        for (std::size_t tone = 1; tone < kToneCount; ++tone) {
            if (gains[tone] > threshold || gains[tone - 1] <= threshold)
                continue;
            const double previous_db = 20.0 * std::log10(gains[tone - 1] / gains.front());
            const double current_db = 20.0 * std::log10(gains[tone] / gains.front());
            const double mix = (-3.01029995664 - previous_db) /
                               (current_db - previous_db);
            crossings[snapshot] = std::exp(
                std::log(frequencies[tone - 1]) + std::clamp(mix, 0.0, 1.0) *
                    (std::log(frequencies[tone]) - std::log(frequencies[tone - 1])));
            break;
        }
        INFO("at " << snapshot_seconds[snapshot] << " s, wet crossing "
                   << crossings[snapshot] << " Hz, clock target " << targets[snapshot]
                   << " Hz");
        REQUIRE(crossings[snapshot] > 0.0);
        CHECK(std::abs(crossings[snapshot] - targets[snapshot]) <=
              0.15 * targets[snapshot]);
    }
    CHECK(targets[1] < targets[0]);
    CHECK(targets[2] < targets[1]);
    CHECK(crossings[1] < crossings[0]);
    CHECK(crossings[2] < crossings[1]);
}

// ═══════════════════════════════════════════════════════════════════════════
// 6 — BBD compander
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("the BBD compander suppresses the line's noise floor in gaps",
          "[character-delay][bbd]") {
    auto measure_gap_floor = [](bool compander) {
        Engine delay;
        configure(delay, Character::bbd, 100.0, 0.0, 1.0);
        delay.set_bbd_compander_enabled(compander);
        delay.reset();
        settle(delay, 0.6);

        // 0.5 s of tone, then 0.5 s of silence; measure the wet output in the
        // second half of the gap, once the delayed tone has also stopped.
        const int n = static_cast<int>(kSr * 1.0);
        auto buffers = sine_both(n, 1000.0, 0.5f);
        for (int i = n / 2; i < n; ++i) {
            buffers.left[static_cast<std::size_t>(i)] = 0.0f;
            buffers.right[static_cast<std::size_t>(i)] = 0.0f;
        }
        render(delay, buffers);
        return rms(buffers.left, static_cast<int>(kSr * 0.8), n);
    };

    const double with_compander = measure_gap_floor(true);
    const double without = measure_gap_floor(false);
    INFO("gap floor with " << with_compander << " without " << without);
    CHECK(20.0 * std::log10((with_compander + 1e-15) / (without + 1e-15)) < -10.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 7 — Tape wow and flutter
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("tape instability appears at the modeled rates and vanishes at zero",
          "[character-delay][tape][slow]") {
    constexpr double kCarrier = 1000.0;
    constexpr double kDelayMs = 500.0;

    auto delay_deviation_ms = [](double character_amount, std::vector<double>& track) {
        Engine delay;
        configure(delay, Character::tape, kDelayMs, 0.0, character_amount);
        settle(delay, 4.0 * slew_seconds(Character::tape) + 0.6);

        const int n = static_cast<int>(kSr * 20.0);
        auto buffers = sine_both(n, kCarrier, 0.25f);
        render(delay, buffers);
        // Skip the first second so the demodulator's own filters have settled.
        track = phase_track(buffers.left, static_cast<int>(kSr), n, kCarrier);

        double mean = 0.0;
        for (double p : track) mean += p;
        mean /= static_cast<double>(track.size());
        double variance = 0.0;
        for (double& p : track) {
            p -= mean;
            variance += p * p;
        }
        variance /= static_cast<double>(track.size());
        // Phase deviation back to a read-head displacement in milliseconds.
        return 1000.0 * std::sqrt(variance) / (2.0 * cd::kPi * kCarrier);
    };

    std::vector<double> unstable_track;
    std::vector<double> stable_track;
    const double moving = delay_deviation_ms(0.67, unstable_track);
    const double flat = delay_deviation_ms(0.0, stable_track);
    REQUIRE(unstable_track.size() > 1000);

    INFO("read-head deviation at character 0.67: " << moving << " ms, at 0: " << flat << " ms");
    CHECK(moving > 10.0 * flat);

    // Magnitude consistent with the shipped depths. Wow and flutter are
    // independent, so their RMS contributions add in quadrature; the tolerance
    // is wide because the drift term is stochastic by design.
    const double wow = cd::interpolate_knots(cd::kTapeAxis, cd::kTapeWowDepthMs, 0.67);
    const double flutter =
        cd::interpolate_knots(cd::kTapeAxis, cd::kTapeFlutterDepthMs, 0.67);
    const double nominal = std::sqrt(0.5 * wow * wow + 0.5 * flutter * flutter);
    INFO("nominal deviation " << nominal << " ms");
    CHECK(moving > 0.5 * nominal);
    CHECK(moving < 2.0 * nominal);

    // The deviation's own spectrum must peak at the modeled wow rate.
    std::vector<float> as_float(unstable_track.size());
    for (std::size_t i = 0; i < unstable_track.size(); ++i)
        as_float[i] = static_cast<float>(unstable_track[i]);

    const auto psd = welch_psd(as_float, 0, static_cast<int>(as_float.size()), 4096);
    std::size_t best = 1;
    for (std::size_t bin = 1; bin < psd.size() && psd_bin_hz(bin, psd.size(), kTrackRate) < 4.0;
         ++bin)
        if (psd[bin] > psd[best]) best = bin;
    const double peak_hz = psd_bin_hz(best, psd.size(), kTrackRate);
    INFO("wow peak at " << peak_hz << " Hz, expected " << cd::kWowRateHz);
    CHECK(std::abs(peak_hz - cd::kWowRateHz) <= 0.2);

    // Flutter is a deliberate three-harmonic rotational stack. Integrate a
    // narrow band around each line rather than reading one FFT bin so a line
    // half-way between bins cannot fail from leakage alone.
    const auto band_power = [&](double centre_hz) {
        double power = 0.0;
        for (std::size_t bin = 1; bin < psd.size(); ++bin) {
            const double hz = psd_bin_hz(bin, psd.size(), kTrackRate);
            if (std::abs(hz - centre_hz) <= 0.55) power += psd[bin];
        }
        return power;
    };
    const std::array<double, 3> flutter_power = {
        band_power(cd::kFlutterBaseHz), band_power(2.0 * cd::kFlutterBaseHz),
        band_power(3.0 * cd::kFlutterBaseHz)};
    INFO("flutter powers " << flutter_power[0] << ", " << flutter_power[1] << ", "
                            << flutter_power[2]);
    CHECK(flutter_power[0] > flutter_power[1]);
    CHECK(flutter_power[1] > flutter_power[2]);
}

TEST_CASE("tape modulation does not shift the mean delay", "[character-delay][tape]") {
    // The mean-delay rule: instability changes pitch, never tempo. Measured by
    // where the repeat of an impulse lands with instability at maximum.
    Engine delay;
    configure(delay, Character::tape, 500.0, 0.0, 1.0);
    settle(delay, 4.0 * slew_seconds(Character::tape) + 0.6);

    double total = 0.0;
    const int trials = 64;
    for (int trial = 0; trial < trials; ++trial) {
        auto buffers = impulse_left(static_cast<int>(kSr * 1.2));
        render(delay, buffers);
        total += peak_index(buffers.left, 1, static_cast<int>(kSr * 1.1));
    }
    const double mean_index = total / trials;
    const double expected = 0.5 * kSr;
    INFO("mean repeat index " << mean_index << ", expected " << expected);
    CHECK(std::abs(mean_index - expected) <= 0.0001 * kSr);  // +-0.1 ms
}

// ═══════════════════════════════════════════════════════════════════════════
// 8 — Vintage floor and darkening
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("vintage band-limits to its internal rate", "[character-delay][vintage]") {
    Engine delay;
    configure(delay, Character::vintage_digital, 200.0, 0.0, 0.5);
    settle(delay, 1.0);

    // The reported edge must be the table's fraction of the reported internal
    // rate — a real relationship, not a value compared against itself.
    const double edge = delay.vintage_band_edge_hz();
    INFO("reported band edge " << edge);
    CHECK(edge == Catch::Approx(cd::kVintageAntiAliasFraction * delay.vintage_internal_rate_hz()));

    auto response_at = [&](double hz) {
        Engine probe;
        configure(probe, Character::vintage_digital, 200.0, 0.0, 0.5);
        settle(probe, 1.0);
        const int n = static_cast<int>(kSr * 1.0);
        auto buffers = sine_both(n, hz, 0.5f);
        render(probe, buffers);
        return magnitude_at(buffers.left, static_cast<int>(kSr * 0.5), n, hz);
    };

    const double passband = response_at(300.0);
    const double at_edge = response_at(edge);
    const double beyond = response_at(std::min(edge * 1.8, 0.45 * kSr));
    INFO("passband " << passband << " edge " << at_edge << " beyond " << beyond);
    CHECK(at_edge < passband);
    CHECK(beyond < at_edge * 0.5);
}

TEST_CASE("vintage wet audio crosses minus 3 dB at its converter edge",
          "[character-delay][vintage][slow]") {
    Engine law;
    configure(law, Character::vintage_digital, 200.0, 0.0, 0.5);
    settle(law, 1.0);
    const double expected = cd::kVintageAntiAliasFraction * law.vintage_internal_rate_hz();
    const auto response = measure_minus_3db_crossing(
        expected, [](double hz) {
            return measured_wet_tone_gain(Character::vintage_digital, 200.0, 0.5, hz);
        });
    REQUIRE(response.reference_gain > 1e-6);
    REQUIRE_FALSE(response.probes.empty());
    UNSCOPED_INFO("response reference " << response.reference_hz
                                        << " Hz=" << response.reference_gain
                                        << ", probe " << response.probes.front().first
                                        << " Hz=" << response.probes.front().second);
    for (std::size_t index = 1; index < response.probes.size(); ++index) {
        const auto& probe = response.probes[index];
        UNSCOPED_INFO("response probe " << probe.first << " Hz=" << probe.second);
    }
    INFO("measured wet crossing " << response.crossing_hz
                                  << " Hz, converter target " << expected << " Hz");
    REQUIRE(response.crossing_hz > 0.0);
    CHECK(std::abs(response.crossing_hz - expected) <= 0.10 * expected);
}

TEST_CASE("vintage repeats darken as they recirculate", "[character-delay][vintage]") {
    Engine delay;
    configure(delay, Character::vintage_digital, 200.0, 0.6, 0.5);
    settle(delay, 1.0);

    // Broadband in: a narrow-band burst has no high frequencies for the
    // converter loop to shed, so it cannot show darkening even when it happens.
    auto buffers = impulse_left(static_cast<int>(kSr * 1.2), 0.9f);
    render(delay, buffers);

    double previous = 1e12;
    for (int repeat = 1; repeat <= 4; ++repeat) {
        const int centre = static_cast<int>(repeat * 0.2 * kSr);
        const double centroid = spectral_centroid(buffers.left, centre - 2048, centre + 2048);
        INFO("repeat " << repeat << " centroid " << centroid);
        CHECK(centroid < previous);
        previous = centroid;
    }
}

TEST_CASE("vintage quantization noise sits in the dithered PCM region",
          "[character-delay][vintage]") {
    // At the 12-bit knot a -60 dBFS tone must come back with a noise floor set
    // by the quantizer and its TPDF dither, not by the signal.
    Engine delay;
    configure(delay, Character::vintage_digital, 100.0, 0.0, 0.5);
    settle(delay, 1.0);

    const int n = static_cast<int>(kSr * 2.0);
    auto buffers = sine_both(n, 1000.0, static_cast<float>(std::pow(10.0, -60.0 / 20.0)));
    render(delay, buffers);

    const auto psd = welch_psd(buffers.left, static_cast<int>(kSr), n, 32768);
    // Integrate everything except the tone's own bins.
    double noise = 0.0;
    for (std::size_t bin = 1; bin < psd.size(); ++bin) {
        const double f = psd_bin_hz(bin, psd.size(), kSr);
        if (std::abs(f - 1000.0) < 50.0) continue;
        noise += psd[bin];
    }
    // Welch uses an unnormalized FFT. Divide the integrated one-sided power
    // by the Hann window's energy and segment length to recover mean-square
    // amplitude in full-scale units. Interior positive-frequency bins carry
    // the matching negative-frequency energy, hence the factor of two.
    constexpr double kHannMeanSquare = 3.0 / 8.0;
    const double segment = 32768.0;
    const double noise_mean_square = 2.0 * noise / (segment * segment * kHannMeanSquare);
    const double noise_db = 10.0 * std::log10(std::max(noise_mean_square, 1e-30));
    INFO("integrated wet noise floor " << noise_db << " dBFS");
    CHECK(noise_db >= -80.0);
    CHECK(noise_db <= -68.0);
}

TEST_CASE("vintage zero-depth delay has no fractional-clock phase warble",
          "[character-delay][vintage][phase]") {
    auto measure = [](double modulation_seconds) {
        cd::VintageChannel vintage;
        vintage.prepare(kSr);
        vintage.set_seed(0x12345678u);
        vintage.update(0.0);
        vintage.reset();

        constexpr double kCarrierHz = 4000.0;
        constexpr double kDelaySeconds = 0.137123;
        constexpr int kDuration = static_cast<int>(6.0 * kSr);
        std::vector<float> output(static_cast<std::size_t>(kDuration));
        for (int i = 0; i < kDuration; ++i) {
            const double t = static_cast<double>(i) / kSr;
            const double input = 0.3 * std::sin(2.0 * cd::kPi * kCarrierHz * t);
            const double delay =
                kDelaySeconds + modulation_seconds * std::sin(2.0 * cd::kPi * 0.7 * t);
            output[static_cast<std::size_t>(i)] = static_cast<float>(vintage.process(input, delay));
        }
        const auto phase = phase_track(output, static_cast<int>(2.0 * kSr), kDuration, kCarrierHz);
        return detrended_phase_rms(phase);
    };

    const double zero_depth = measure(0.0);
    const double modulated_control = measure(0.0005);
    INFO("zero-depth phase residual " << zero_depth << ", modulated control " << modulated_control);
    CHECK(zero_depth < 0.02);
    CHECK(modulated_control > 5.0 * zero_depth);
}

TEST_CASE("vintage Lagrange read keeps gain stable across fractional clock phase",
          "[character-delay][vintage][phase]") {
    const double low_frequency = vintage_fractional_phase_rms(1001.0);
    const double near_band_edge = vintage_fractional_phase_rms(11003.0);
    INFO("fractional-phase RMS spread low " << low_frequency << ", near edge " << near_band_edge);
    // The modeled converter and reconstruction filter retain clock-phase
    // coloration near the edge; this is not expected to be an ideal sinc DAC.
    // The previous two-point read measures 0.719 here. The four-point Lagrange
    // path measures 0.660 and stays below this deliberately separated bound.
    CHECK(low_frequency < 0.01);
    CHECK(near_band_edge < 0.69);
}

// ═══════════════════════════════════════════════════════════════════════════
// 9 — Diffusion
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("diffusion smears a repeat into a cluster", "[character-delay][diffusion]") {
    auto count_above = [](Character character) {
        Engine delay;
        configure(delay, character, 100.0, 0.0, 0.5);
        settle(delay, 0.3);
        auto buffers = impulse_left(static_cast<int>(kSr * 0.4));
        render(delay, buffers);

        const int centre = peak_index(buffers.left, 1, static_cast<int>(buffers.left.size()));
        const int span = static_cast<int>(0.03 * kSr);
        const double top = peak(buffers.left, centre - span, centre + span);
        int count = 0;
        for (int i = std::max(1, centre - span);
             i < centre + span && i < static_cast<int>(buffers.left.size()); ++i)
            if (std::abs(static_cast<double>(buffers.left[static_cast<std::size_t>(i)])) >
                0.05 * top)
                ++count;
        return count;
    };

    const int clean = count_above(Character::clean);
    const int diffused = count_above(Character::diffusion);
    INFO("clean " << clean << " diffused " << diffused);
    CHECK(diffused >= 8 * std::max(clean, 1));
}

TEST_CASE("diffusion blooms into a reverb tail as the character opens",
          "[character-delay][diffusion]") {
    // An allpass diffuser alone cannot make a reverb: it has no recirculation,
    // so it smears a transient over tens of milliseconds and is then finished.
    // The tank gives the character its own short-time-constant recirculation,
    // so energy keeps arriving long after the smear has passed.
    //
    // The measurement has to be a LATE window for that reason. Comparing total
    // tail energy at amount 0 against amount 1 proves nothing about the tank —
    // the input diffuser's own size and gain scaling already moves that by
    // orders of magnitude. A second after the transient is somewhere only a
    // recirculating network can put energy.
    //
    // Delay feedback is ZERO throughout, so nothing here is the delay's tail.
    auto windows = [](double amount) {
        Engine delay;
        configure(delay, Character::diffusion, 100.0, 0.0, amount);
        settle(delay, 0.3);
        auto buffers = impulse_left(static_cast<int>(kSr * 2.5));
        render(delay, buffers);

        auto energy = [&](double from_s, double to_s) {
            double e = 0.0;
            const auto from = static_cast<std::size_t>(from_s * kSr);
            const auto to = std::min(static_cast<std::size_t>(to_s * kSr), buffers.left.size());
            for (std::size_t i = from; i < to; ++i)
                e += static_cast<double>(buffers.left[i]) * buffers.left[i];
            return e;
        };
        return std::pair{energy(0.25, 0.5), energy(0.8, 2.0)};
    };

    const auto [early_open, late_open] = windows(1.0);
    INFO("open: early " << early_open << " late " << late_open);
    REQUIRE(early_open > 0.0);
    // A second later the cloud still holds a substantial share of the energy it
    // had half a second in. The bare diffuser misses this by seven orders of
    // magnitude, so the threshold is not delicately placed.
    CHECK(late_open > early_open * 0.05);

    // At amount 0 the tank is bypassed outright, which is what keeps the bare
    // published diffuser available as the magnitude-flat baseline.
    const auto [early_closed, late_closed] = windows(0.0);
    INFO("closed: early " << early_closed << " late " << late_closed);
    CHECK(late_closed < early_closed * 1e-3);
}

TEST_CASE("diffusion holds its cloud together without the delay's feedback",
          "[character-delay][diffusion]") {
    // The output-only tank recirculates on its own, so the character must still
    // decay to silence on its own — a reverb, not an oscillator. Its energy
    // never re-enters the delay line; this test owns the tank's independent
    // stability contract.
    Engine delay;
    configure(delay, Character::diffusion, 100.0, 0.0, 1.0);
    settle(delay, 0.3);

    auto buffers = impulse_left(static_cast<int>(kSr * 8.0));
    render(delay, buffers);

    for (float v : buffers.left) CHECK(std::isfinite(v));

    // Energy must fall monotonically-ish across the tail, not sustain or grow.
    auto window_energy = [&](double from_s, double to_s) {
        double e = 0.0;
        const auto from = static_cast<std::size_t>(from_s * kSr);
        const auto to = std::min(static_cast<std::size_t>(to_s * kSr), buffers.left.size());
        for (std::size_t i = from; i < to; ++i) e += static_cast<double>(buffers.left[i]) *
                                                    buffers.left[i];
        return e;
    };
    const double early = window_energy(0.3, 1.0);
    const double late = window_energy(5.0, 6.0);
    INFO("early " << early << " late " << late);
    CHECK(early > 0.0);
    CHECK(late < early * 0.1);
}

TEST_CASE("diffusion at zero amount adds no tail of its own",
          "[character-delay][diffusion]") {
    // The tank is bypassed entirely at amount 0, which is what keeps the bare
    // published diffuser available as the magnitude-flat baseline.
    auto tail_energy = [](Character character) {
        Engine delay;
        configure(delay, character, 100.0, 0.0, 0.0);
        settle(delay, 0.3);
        auto buffers = impulse_left(static_cast<int>(kSr * 2.0));
        render(delay, buffers);
        double e = 0.0;
        for (std::size_t i = static_cast<std::size_t>(0.5 * kSr); i < buffers.left.size(); ++i)
            e += static_cast<double>(buffers.left[i]) * buffers.left[i];
        return e;
    };
    // With no tank and no feedback, diffusion must ring out much like clean.
    const double clean = tail_energy(Character::clean);
    const double diffused = tail_energy(Character::diffusion);
    INFO("clean " << clean << " diffusion " << diffused);
    CHECK(diffused < std::max(clean, 1e-12) * 50.0);
}

TEST_CASE("diffusion at maximum feedback blooms but never runs away",
          "[character-delay][diffusion]") {
    // The tank recirculates internally on the wet OUTPUT, outside the delay's
    // feedback loop. Its output level still sits on a cliff: a resonant mode
    // above unity would make one repeat's cloud run away even though that
    // energy never returns to the delay line. This pins the stable side of that
    // cliff at maximum feedback and character amount.
    Engine delay;
    configure(delay, Character::diffusion, 100.0, 1.0, 1.0);
    settle(delay, 0.3);

    const int drive = static_cast<int>(kSr * 3.0);
    const int release = static_cast<int>(kSr * 7.0);
    auto buffers = make_stereo(drive + release);
    for (int i = 0; i < drive; ++i) {
        const auto v = static_cast<float>(
            0.3 * std::sin(2.0 * cd::kPi * 1000.0 * i / kSr));
        buffers.left[static_cast<std::size_t>(i)] = v;
        buffers.right[static_cast<std::size_t>(i)] = v;
    }
    render(delay, buffers);

    double driven = 0.0, bloom = 0.0;
    for (int i = 0; i < drive; ++i)
        driven = std::max(driven, std::abs(static_cast<double>(
            buffers.left[static_cast<std::size_t>(i)])));
    for (int i = drive; i < drive + release; ++i) {
        const double v = buffers.left[static_cast<std::size_t>(i)];
        CHECK(std::isfinite(v));
        bloom = std::max(bloom, std::abs(v));
    }
    INFO("driven peak " << driven << " release peak " << bloom);
    REQUIRE(driven > 1e-3);
    // Dub-style bloom after release is legitimate; a runaway is not. The
    // stable tuning measures ~2x, the first unstable one ~16x, the cliff 300x.
    CHECK(bloom < driven * 8.0);
}

TEST_CASE("the diffuser is allpass in steady state", "[character-delay][diffusion]") {
    // Measured at character amount ZERO, which is where the flatness contract
    // lives: the tank is bypassed outright there and the character is the bare
    // published Dattorro diffuser — the null-test baseline. Above zero the
    // character now deliberately blooms into a damped cloud, and a damped
    // recirculation is not magnitude-flat; asking for flatness at 0.5 would
    // forbid the reverb the character exists to produce.
    Engine delay;
    configure(delay, Character::diffusion, 20.0, 0.0, 0.0);
    settle(delay, 0.3);

    // White noise in; the allpass chain must not change the magnitude response
    // between the loop filters' corners.
    const int n = static_cast<int>(kSr * 4.0);
    auto buffers = make_stereo(n);
    cd::Xorshift32 rng(12345u);
    for (int i = 0; i < n; ++i) {
        const auto v = static_cast<float>(0.2 * rng.bipolar());
        buffers.left[static_cast<std::size_t>(i)] = v;
        buffers.right[static_cast<std::size_t>(i)] = v;
    }
    render(delay, buffers);

    const auto psd = welch_psd(buffers.left, static_cast<int>(kSr), n, 8192);
    double low = 1e30;
    double high = 0.0;
    for (std::size_t bin = 1; bin < psd.size(); ++bin) {
        const double f = psd_bin_hz(bin, psd.size(), kSr);
        if (f < 100.0 || f > 8000.0) continue;
        low = std::min(low, psd[bin]);
        high = std::max(high, psd[bin]);
    }
    // Compare smoothed octave-band energies rather than raw bins: a single
    // realization of noise has its own several-dB bin-to-bin scatter, which is
    // a property of the stimulus, not of the filter under test.
    double worst = 0.0;
    double reference = 0.0;
    for (double centre = 141.0; centre < 8000.0; centre *= 2.0) {
        double band = 0.0;
        int count = 0;
        for (std::size_t bin = 1; bin < psd.size(); ++bin) {
            const double f = psd_bin_hz(bin, psd.size(), kSr);
            if (f < centre / std::sqrt(2.0) || f > centre * std::sqrt(2.0)) continue;
            band += psd[bin];
            ++count;
        }
        if (count == 0) continue;
        band /= count;
        if (reference == 0.0) reference = band;
        worst = std::max(worst, std::abs(10.0 * std::log10(band / reference)));
    }
    INFO("worst octave-band deviation " << worst << " dB (raw bin spread "
                                        << 10.0 * std::log10(high / std::max(low, 1e-30)) << ")");
    CHECK(worst < 1.5);
}

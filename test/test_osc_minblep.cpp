// Fixed-capacity minBLEP accumulator contracts and end-to-end alias residuals.

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/osc/blep.hpp>
#include <pulp/signal/osc/minblep.hpp>
#include <pulp/signal/osc/phase.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::osc::Correction;
using pulp::signal::osc::MinBlepAccumulator;
using pulp::signal::osc::MinBlepInsertResult;
using pulp::signal::osc::PhaseAccumulator;
using pulp::signal::osc::poly_blep;
using pulp::signal::osc::wrap_position;
using pulp::test::audio::AliasOptions;
using pulp::test::audio::AliasReport;
using pulp::test::audio::measure_aliasing;

namespace {

constexpr double kSampleRate = 48'000.0;
constexpr int kFitLength = 8192;
constexpr int kWarmup = 128;
constexpr double kBandHz = 20'000.0;
constexpr double kTestF0[] = {1103.0, 2153.0, 4100.0, 6301.0};

enum class CorrectionKind { polyblep, minblep };

int harmonics_for(double f0) {
    return static_cast<int>(std::ceil(3.0 * kSampleRate / f0));
}

double saw(double phase) {
    return 2.0 * phase - 1.0;
}

double saw_limit(double phase) {
    if (phase == 1.0)
        return 1.0;
    return saw(phase);
}

AliasReport analyze(const std::vector<double>& signal, double f0) {
    pulp::audio::Buffer<float> buffer(1, static_cast<int>(signal.size()));
    for (int index = 0; index < static_cast<int>(signal.size()); ++index)
        buffer.channel(0)[index] = static_cast<float>(signal[static_cast<std::size_t>(index)]);

    AliasOptions options;
    options.num_harmonics = harmonics_for(f0);
    options.analysis_length = static_cast<int>(signal.size());
    options.max_alias_frequency_hz = kBandHz;
    return measure_aliasing(std::as_const(buffer).view(), f0, kSampleRate, options);
}

void require_trustworthy(const AliasReport& report) {
    REQUIRE_FALSE(report.has_unresolved_in_band_alias);
    REQUIRE(report.worst_alias_db > report.detection_floor_db);
}

double upper_quarter_energy_of_difference(const std::vector<double>& signal) {
    std::vector<double> difference(signal.size(), 0.0);
    double previous = 0.0;
    for (std::size_t index = 0; index < signal.size(); ++index) {
        difference[index] = signal[index] - previous;
        previous = signal[index];
    }

    double energy = 0.0;
    const std::size_t first_bin = signal.size() / 4;
    const std::size_t nyquist_bin = signal.size() / 2;
    for (std::size_t bin = first_bin; bin <= nyquist_bin; ++bin) {
        std::complex<double> projection{};
        for (std::size_t index = 0; index < difference.size(); ++index) {
            const double angle = -2.0 * std::numbers::pi * static_cast<double>(bin * index) /
                                 static_cast<double>(difference.size());
            projection += difference[index] * std::polar(1.0, angle);
        }
        energy += std::norm(projection);
    }
    return energy;
}

std::vector<double> render_saw(double f0, CorrectionKind kind) {
    const int total = kFitLength + kWarmup;
    const double increment = f0 / kSampleRate;
    std::vector<double> rendered(static_cast<std::size_t>(total), 0.0);
    MinBlepAccumulator<> accumulator;
    double phase = 0.25;
    double carry = 0.0;

    for (int index = 0; index < total; ++index) {
        double sample = saw(phase);
        if (kind == CorrectionKind::minblep)
            sample += accumulator.next();
        else {
            sample += carry;
            carry = 0.0;
        }

        phase += increment;
        if (phase >= 1.0) {
            phase -= 1.0;
            const double position = wrap_position(phase, increment);
            if (kind == CorrectionKind::minblep) {
                REQUIRE(accumulator.insert(position, -2.0) == MinBlepInsertResult::inserted);
            } else {
                const Correction correction = poly_blep(position, -2.0);
                sample += correction.before;
                carry += correction.after;
            }
        }
        rendered[static_cast<std::size_t>(index)] = sample;
    }
    return {rendered.begin() + kWarmup, rendered.end()};
}

std::vector<double> render_hard_sync(double master_f0, double slave_f0, CorrectionKind kind) {
    const int total = kFitLength + kWarmup;
    const double master_increment = master_f0 / kSampleRate;
    const double slave_increment = slave_f0 / kSampleRate;
    std::vector<double> rendered(static_cast<std::size_t>(total), 0.0);
    MinBlepAccumulator<> accumulator;
    PhaseAccumulator slave;
    slave.reset(0.37);
    double master_phase = 0.13;
    double carry = 0.0;

    for (int index = 0; index < total; ++index) {
        double sample = saw(slave.phase());
        if (kind == CorrectionKind::minblep)
            sample += accumulator.next();
        else {
            sample += carry;
            carry = 0.0;
        }

        master_phase += master_increment;
        if (master_phase >= 1.0) {
            master_phase -= 1.0;
            slave.advance_synced(slave_increment, wrap_position(master_phase, master_increment),
                                 0.0);
        } else {
            slave.advance(slave_increment);
        }

        for (const auto& event : slave.events()) {
            const double height = saw_limit(event.phase_after) - saw_limit(event.phase_before);
            if (kind == CorrectionKind::minblep) {
                REQUIRE(accumulator.insert(event.frac, height) !=
                        MinBlepInsertResult::capacity_exceeded);
            } else {
                const Correction correction = poly_blep(event.frac, height);
                sample += correction.before;
                carry += correction.after;
            }
        }
        rendered[static_cast<std::size_t>(index)] = sample;
    }
    return {rendered.begin() + kWarmup, rendered.end()};
}

struct PartitionedSaw {
    explicit PartitionedSaw(double frequency) : increment(frequency / kSampleRate) {}

    void render(std::span<double> output) {
        for (double& sample : output) {
            sample = saw(phase) + accumulator.next();
            phase += increment;
            if (phase >= 1.0) {
                phase -= 1.0;
                REQUIRE(accumulator.insert(wrap_position(phase, increment), -2.0) ==
                        MinBlepInsertResult::inserted);
            }
        }
    }

    double increment;
    double phase = 0.25;
    MinBlepAccumulator<> accumulator;
};

} // namespace

TEST_CASE("minBLEP insertion validates inputs and pins fractional endpoints",
          "[signal][osc][minblep]") {
    MinBlepAccumulator<> accumulator;
    REQUIRE(accumulator.insert(-0.01, 1.0) == MinBlepInsertResult::invalid_position);
    REQUIRE(accumulator.insert(1.01, 1.0) == MinBlepInsertResult::invalid_position);
    REQUIRE(accumulator.insert(std::numeric_limits<double>::quiet_NaN(), 1.0) ==
            MinBlepInsertResult::invalid_position);
    REQUIRE(accumulator.insert(0.5, std::numeric_limits<double>::infinity()) ==
            MinBlepInsertResult::non_finite_height);
    REQUIRE(accumulator.insert(0.5, 0.0) == MinBlepInsertResult::zero_height);
    REQUIRE(accumulator.active_events() == 0);

    MinBlepAccumulator<> at_start;
    MinBlepAccumulator<> at_end;
    REQUIRE(at_start.insert(0.0, 1.0) == MinBlepInsertResult::inserted);
    REQUIRE(at_end.insert(1.0, 1.0) == MinBlepInsertResult::inserted);
    REQUIRE(at_start.next() != at_end.next());
}

TEST_CASE("minBLEP response is finite linear and converges to exact zero",
          "[signal][osc][minblep]") {
    MinBlepAccumulator<> unit;
    MinBlepAccumulator<> scaled;
    REQUIRE(unit.insert(0.375, 1.0) == MinBlepInsertResult::inserted);
    REQUIRE(scaled.insert(0.375, -2.0) == MinBlepInsertResult::inserted);

    for (std::size_t index = 0; index < MinBlepAccumulator<>::kernel_samples; ++index) {
        const double value = unit.next();
        const double scaled_value = scaled.next();
        REQUIRE(std::isfinite(value));
        REQUIRE_THAT(scaled_value, WithinAbs(-2.0 * value, 1.0e-12));
    }
    REQUIRE(unit.active_events() == 0);
    REQUIRE(scaled.active_events() == 0);
    REQUIRE(unit.next() == 0.0);
    REQUIRE(scaled.next() == 0.0);
}

TEST_CASE("minBLEP bandlimits an arbitrary isolated discontinuity",
          "[signal][osc][minblep][impulse]") {
    constexpr std::size_t sample_count = 256;
    constexpr std::size_t event_sample = 63;
    constexpr double position = 0.371;
    constexpr double height = 0.37;
    MinBlepAccumulator<> accumulator;
    std::vector<double> trivial(sample_count, 0.0);
    std::vector<double> corrected(sample_count, 0.0);

    for (std::size_t index = 0; index < sample_count; ++index) {
        const double stepped = index > event_sample ? height : 0.0;
        trivial[index] = stepped;
        corrected[index] = stepped + accumulator.next();
        if (index == event_sample)
            REQUIRE(accumulator.insert(position, height) == MinBlepInsertResult::inserted);
    }

    const double trivial_high_band = upper_quarter_energy_of_difference(trivial);
    const double corrected_high_band = upper_quarter_energy_of_difference(corrected);
    INFO("arbitrary step high-band energy trivial=" << trivial_high_band
                                                     << " corrected=" << corrected_high_band);
    REQUIRE(corrected_high_band < trivial_high_band * 0.65);
    REQUIRE(corrected.back() == height);
    REQUIRE(accumulator.active_events() == 0);
}

TEST_CASE("minBLEP collision overflow drops only the new event deterministically",
          "[signal][osc][minblep]") {
    MinBlepAccumulator<2> full;
    MinBlepAccumulator<2> control;
    for (auto* accumulator : {&full, &control}) {
        REQUIRE(accumulator->insert(0.25, 1.0) == MinBlepInsertResult::inserted);
        REQUIRE(accumulator->insert(0.75, -0.5) == MinBlepInsertResult::inserted);
    }
    REQUIRE(full.insert(0.5, 1000.0) == MinBlepInsertResult::capacity_exceeded);
    REQUIRE(full.active_events() == 2);

    for (std::size_t index = 0; index < MinBlepAccumulator<2>::kernel_samples; ++index)
        REQUIRE(full.next() == control.next());
}

TEST_CASE("minBLEP reset clears every pending correction", "[signal][osc][minblep]") {
    MinBlepAccumulator<> accumulator;
    REQUIRE(accumulator.insert(0.5, 1.0) == MinBlepInsertResult::inserted);
    REQUIRE(accumulator.next() != 0.0);
    accumulator.reset();
    REQUIRE(accumulator.active_events() == 0);
    REQUIRE(accumulator.next() == 0.0);
}

TEST_CASE("minBLEP audio-thread operations allocate no memory", "[signal][osc][minblep]") {
    STATIC_REQUIRE(sizeof(MinBlepAccumulator<>) <= 256);
    MinBlepAccumulator<> accumulator;
    pulp::test::RtAllocationProbe probe;
    for (int index = 0; index < 4096; ++index) {
        if ((index % 37) == 0)
            (void)accumulator.insert(0.375, -2.0);
        (void)accumulator.next();
        if ((index % 997) == 0)
            accumulator.reset();
    }
    REQUIRE_FALSE(probe.saw_allocation());
}

TEST_CASE("minBLEP rendering is invariant to block partitioning", "[signal][osc][minblep]") {
    constexpr std::size_t sample_count = 4096;
    std::vector<double> reference(sample_count);
    PartitionedSaw continuous(2153.0);
    continuous.render(reference);

    for (const std::size_t block_size :
         {std::size_t{1}, std::size_t{17}, std::size_t{64}, std::size_t{257}}) {
        std::vector<double> partitioned(sample_count);
        PartitionedSaw renderer(2153.0);
        for (std::size_t begin = 0; begin < sample_count; begin += block_size) {
            const std::size_t count = std::min(block_size, sample_count - begin);
            renderer.render(std::span<double>(partitioned).subspan(begin, count));
        }
        REQUIRE(partitioned == reference);
    }
}

TEST_CASE("minBLEP saw has a lower alias residual than polyBLEP", "[signal][osc][minblep][alias]") {
    for (const double f0 : kTestF0) {
        const AliasReport poly = analyze(render_saw(f0, CorrectionKind::polyblep), f0);
        const AliasReport minimum = analyze(render_saw(f0, CorrectionKind::minblep), f0);
        require_trustworthy(poly);
        require_trustworthy(minimum);
        INFO("f0=" << f0 << " poly=" << poly.worst_alias_db
                   << " dB minBLEP=" << minimum.worst_alias_db << " dB");
        REQUIRE(poly.worst_alias_db - minimum.worst_alias_db >= 30.0);
    }
}

TEST_CASE("minBLEP improves hard-sync alias residual over polyBLEP",
          "[signal][osc][minblep][alias][sync]") {
    constexpr double master_f0 = 1103.0;
    constexpr double slave_f0 = 6301.0;
    const AliasReport poly =
        analyze(render_hard_sync(master_f0, slave_f0, CorrectionKind::polyblep), master_f0);
    const AliasReport minimum =
        analyze(render_hard_sync(master_f0, slave_f0, CorrectionKind::minblep), master_f0);
    require_trustworthy(poly);
    require_trustworthy(minimum);
    INFO("hard sync poly=" << poly.worst_alias_db << " dB minBLEP=" << minimum.worst_alias_db
                           << " dB");
    REQUIRE(poly.worst_alias_db - minimum.worst_alias_db >= 30.0);
}

TEST_CASE("minBLEP remains bounded and reports capacity at the Nyquist limit",
          "[signal][osc][minblep]") {
    MinBlepAccumulator<> accumulator;
    double phase = 0.25;
    constexpr double increment = 0.49;
    std::size_t overflows = 0;
    double peak = 0.0;

    for (int index = 0; index < 4096; ++index) {
        const double sample = saw(phase) + accumulator.next();
        REQUIRE(std::isfinite(sample));
        peak = std::max(peak, std::abs(sample));
        phase += increment;
        if (phase >= 1.0) {
            phase -= 1.0;
            if (accumulator.insert(wrap_position(phase, increment), -2.0) ==
                MinBlepInsertResult::capacity_exceeded)
                ++overflows;
        }
    }

    INFO("Nyquist collision overflows=" << overflows << " peak=" << peak);
    REQUIRE(overflows > 0);
    REQUIRE(peak < 4.0);
}

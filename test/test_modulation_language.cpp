#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/breakpoint_envelope.hpp>
#include <pulp/signal/modulation_curve.hpp>
#include <pulp/signal/rise_fall_generator.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSampleRate = 1000.0;

double raw_stage_oracle(double progress, double curve) {
    const double k = 8.0 * curve;
    if (std::abs(k) < 1.0e-12)
        return progress;
    return (1.0 - std::exp(-k * progress)) / (1.0 - std::exp(-k));
}

template <typename Engine> std::vector<double> render_scalar(Engine engine, std::size_t count) {
    std::vector<double> output(count);
    engine.trigger();
    for (auto& sample : output)
        sample = engine.next();
    return output;
}

template <typename Engine>
std::vector<double> render_partitioned(Engine engine, std::span<const std::size_t> partitions) {
    std::vector<double> output;
    engine.trigger();
    for (const auto size : partitions) {
        const auto begin = output.size();
        output.resize(begin + size);
        engine.process(std::span<double>{output}.subspan(begin, size));
    }
    return output;
}

} // namespace

static_assert(std::is_trivially_copyable_v<ModulationCurve>);
static_assert(std::is_trivially_copyable_v<BreakpointEnvelope64>);
static_assert(sizeof(BreakpointEnvelope64) < 2048);

TEST_CASE("Modulation curves match their closed forms and exact endpoints",
          "[signal][modulation-language][curve]") {
    constexpr std::array<double, 5> progress{{0.0, 0.125, 0.5, 0.875, 1.0}};
    for (const double p : progress) {
        REQUIRE_THAT(interpolate_modulation_curve(2.0, 6.0, p, {}),
                     WithinAbs(2.0 + 4.0 * p, 1.0e-12));

        const ModulationCurve exponential{ModulationCurveShape::exponential, 0.75f};
        const ModulationCurve logarithmic{ModulationCurveShape::logarithmic, 0.75f};
        const double rising_fraction = raw_stage_oracle(p, -6.0 / 8.0);
        const double falling_fraction = raw_stage_oracle(p, 6.0 / 8.0);
        REQUIRE_THAT(interpolate_modulation_curve(2.0, 6.0, p, exponential),
                     WithinAbs(2.0 + 4.0 * rising_fraction, 2.0e-6));
        REQUIRE_THAT(interpolate_modulation_curve(6.0, 2.0, p, exponential),
                     WithinAbs(6.0 - 4.0 * falling_fraction, 2.0e-6));
        REQUIRE_THAT(interpolate_modulation_curve(2.0, 6.0, p, logarithmic),
                     WithinAbs(2.0 + 4.0 * falling_fraction, 2.0e-6));
        REQUIRE_THAT(interpolate_modulation_curve(6.0, 2.0, p, logarithmic),
                     WithinAbs(6.0 - 4.0 * rising_fraction, 2.0e-6));

        const ModulationCurve smooth{ModulationCurveShape::smoothstep, 1.0f};
        const double s = p * p * (3.0 - 2.0 * p);
        REQUIRE_THAT(interpolate_modulation_curve(-3.0, 5.0, p, smooth),
                     WithinAbs(-3.0 + 8.0 * s, 2.0e-6));
    }

    const ModulationCurve hold{ModulationCurveShape::hold, 1.0f};
    REQUIRE(interpolate_modulation_curve(3.0, 9.0, 0.999f, hold) == 3.0);
    REQUIRE(interpolate_modulation_curve(3.0, 9.0, 1.0f, hold) == 9.0);

    for (float p = 0.0f; p <= 1.0f; p += 0.125f) {
        const ModulationCurve exponential{ModulationCurveShape::exponential, 0.6f};
        REQUIRE(interpolate_modulation_curve(0.0f, 1.0f, p, exponential) == curve_rise(p, 0.6f));
        REQUIRE(interpolate_modulation_curve(1.0f, 0.0f, p, exponential) == curve_fall(p, 0.6f));
    }
}

TEST_CASE("Curve sanitization has a finite deterministic fault policy",
          "[signal][modulation-language][fault]") {
    const ModulationCurve bad{ModulationCurveShape::exponential,
                              std::numeric_limits<float>::quiet_NaN()};
    REQUIRE_THAT(interpolate_modulation_curve(0.0, 1.0, 0.5f, bad), WithinAbs(0.5, 1.0e-12));
    REQUIRE(interpolate_modulation_curve(2.0, 4.0, std::numeric_limits<float>::infinity()) == 2.0);
    REQUIRE(interpolate_modulation_curve(std::numeric_limits<double>::quiet_NaN(), 4.0, 0.5f) ==
            4.0);
    REQUIRE(interpolate_modulation_curve(2.0, std::numeric_limits<double>::infinity(), 0.5f) ==
            2.0);
    REQUIRE(interpolate_modulation_curve(0.0, std::numeric_limits<double>::denorm_min(), 1.0) ==
            0.0);
    const ModulationCurve unknown{static_cast<ModulationCurveShape>(0xff), 1.0f};
    REQUIRE(interpolate_modulation_curve(2.0, 6.0, 0.25, unknown) == 3.0);
}

TEST_CASE("Breakpoint segments reach endpoints in exactly their rounded sample time",
          "[signal][modulation-language][timing]") {
    BreakpointEnvelope64 envelope;
    REQUIRE(envelope.prepare(kSampleRate) == BreakpointEnvelopeStatus::ok);
    const std::array<BreakpointEnvelopePoint64, 2> points{{
        {0.0, -1.0, {}},
        {3.0, 2.0, {}},
    }};
    REQUIRE(envelope.configure(points) == BreakpointEnvelopeStatus::ok);
    envelope.trigger();
    REQUIRE(envelope.current() == -1.0);
    REQUIRE_THAT(envelope.next(), WithinAbs(0.0, 1.0e-12));
    REQUIRE_THAT(envelope.next(), WithinAbs(1.0, 1.0e-12));
    REQUIRE_THAT(envelope.next(), WithinAbs(2.0, 1.0e-12));
    REQUIRE_FALSE(envelope.active());
    REQUIRE(envelope.next() == 2.0);
}

TEST_CASE("Breakpoint timing follows the stated rounding contract across sample rates",
          "[signal][modulation-language][timing]") {
    constexpr double duration_ms = 7.3;
    for (const double sample_rate : {44100.0, 48000.0, 192000.0, 768000.0}) {
        BreakpointEnvelope64 envelope;
        REQUIRE(envelope.prepare(sample_rate) == BreakpointEnvelopeStatus::ok);
        const std::array<BreakpointEnvelopePoint64, 2> points{
            {{0.0, 0.0, {}}, {duration_ms, 1.0, {}}}};
        REQUIRE(envelope.configure(points) == BreakpointEnvelopeStatus::ok);
        envelope.trigger();
        const auto expected =
            static_cast<std::uint64_t>(std::llround(duration_ms * sample_rate / 1000.0));
        for (std::uint64_t i = 1; i < expected; ++i) {
            (void)envelope.next();
            REQUIRE(envelope.active());
        }
        REQUIRE_THAT(envelope.next(), WithinAbs(1.0, 1.0e-12));
        REQUIRE_FALSE(envelope.active());
    }
}

TEST_CASE("Zero-time breakpoints are bounded instantaneous transitions",
          "[signal][modulation-language][timing]") {
    BreakpointEnvelope64 envelope;
    REQUIRE(envelope.prepare(kSampleRate) == BreakpointEnvelopeStatus::ok);
    const std::array<BreakpointEnvelopePoint64, 4> points{{
        {0.0, 0.0, {}},
        {0.0, 0.25, {}},
        {0.0, 0.5, {}},
        {2.0, 1.0, {}},
    }};
    REQUIRE(envelope.configure(points) == BreakpointEnvelopeStatus::ok);
    envelope.trigger();
    REQUIRE(envelope.current() == 0.5);
    REQUIRE_THAT(envelope.next(), WithinAbs(0.75, 1.0e-12));
    REQUIRE_THAT(envelope.next(), WithinAbs(1.0, 1.0e-12));
    REQUIRE_FALSE(envelope.active());

    const std::array<BreakpointEnvelopePoint64, 2> zero_loop{{
        {0.0, -1.0, {}},
        {0.0, 1.0, {}},
    }};
    REQUIRE(envelope.configure(zero_loop) == BreakpointEnvelopeStatus::ok);
    REQUIRE(envelope.set_loop(0, 1, BreakpointEnvelope64::kLoopForever) ==
            BreakpointEnvelopeStatus::ok);
    envelope.trigger();
    REQUIRE_FALSE(envelope.active());
    const auto quiescent_value = envelope.current();
    REQUIRE(std::isfinite(quiescent_value));
    REQUIRE(envelope.next() == quiescent_value);
}

TEST_CASE("Breakpoint configuration is transactional and rejects invalid domains",
          "[signal][modulation-language][fault]") {
    BreakpointEnvelope64 envelope;
    const auto original = envelope.points();
    REQUIRE(original.size() == 2);
    const auto original_end = original.back().value;

    REQUIRE(envelope.configure(std::span<const BreakpointEnvelopePoint64>{}) ==
            BreakpointEnvelopeStatus::too_few_points);
    BreakpointEnvelopeT<double, 2> small_envelope;
    const std::array<BreakpointEnvelopePoint64, 3> too_many{{
        {0.0, 0.0, {}},
        {1.0, 0.5, {}},
        {2.0, 1.0, {}},
    }};
    REQUIRE(small_envelope.configure(too_many) == BreakpointEnvelopeStatus::too_many_points);

    const std::array<BreakpointEnvelopePoint64, 2> reversed{{{0.0, 0.0, {}}, {10.0, 1.0, {}}}};
    auto invalid = reversed;
    invalid[0].time_ms = 2.0;
    REQUIRE(envelope.configure(invalid) == BreakpointEnvelopeStatus::invalid_time);
    REQUIRE(envelope.points().back().value == original_end);

    invalid = reversed;
    invalid[1].time_ms = -1.0;
    REQUIRE(envelope.configure(invalid) == BreakpointEnvelopeStatus::invalid_time);
    const std::array<BreakpointEnvelopePoint64, 3> unordered{{
        {0.0, 0.0, {}},
        {2.0, 0.5, {}},
        {1.0, 1.0, {}},
    }};
    REQUIRE(envelope.configure(unordered) == BreakpointEnvelopeStatus::times_not_ordered);
    invalid = reversed;
    invalid[1].value = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(envelope.configure(invalid) == BreakpointEnvelopeStatus::invalid_value);
    REQUIRE(envelope.prepare(std::numeric_limits<double>::infinity()) ==
            BreakpointEnvelopeStatus::invalid_sample_rate);
    REQUIRE(envelope.sample_rate() == BreakpointEnvelope64::kDefaultSampleRate);

    REQUIRE(envelope.configure(reversed) == BreakpointEnvelopeStatus::ok);
    envelope.trigger();
    (void)envelope.next();
    const auto segment_before = envelope.current_segment();
    const auto position_before = envelope.segment_position_samples();
    const auto value_before = envelope.current();
    const auto active_before = envelope.active();
    REQUIRE(envelope.set_loop(1, 1, 1) == BreakpointEnvelopeStatus::invalid_loop);
    REQUIRE(envelope.current_segment() == segment_before);
    REQUIRE(envelope.segment_position_samples() == position_before);
    REQUIRE(envelope.current() == value_before);
    REQUIRE(envelope.active() == active_before);
    REQUIRE(envelope.set_loop(0, 1, 1) == BreakpointEnvelopeStatus::ok);
    REQUIRE(envelope.set_loop(99, 100, 0) == BreakpointEnvelopeStatus::ok);
}

TEST_CASE("Breakpoint rendering is bit-identical across block partitions and reset",
          "[signal][modulation-language][determinism]") {
    BreakpointEnvelopeT<double, 5> envelope;
    REQUIRE(envelope.prepare(48000.0) == BreakpointEnvelopeStatus::ok);
    const std::array<BreakpointEnvelopePoint64, 5> points{{
        {0.0, -0.75, {ModulationCurveShape::exponential, 0.4f}},
        {1.75, 0.5, {ModulationCurveShape::smoothstep, 1.0f}},
        {3.0, -0.25, {ModulationCurveShape::logarithmic, 0.8f}},
        {4.25, 1.0, {ModulationCurveShape::hold, 1.0f}},
        {6.0, 0.125, {}},
    }};
    REQUIRE(envelope.configure(points) == BreakpointEnvelopeStatus::ok);

    const auto scalar = render_scalar(envelope, 400);
    constexpr std::array<std::size_t, 8> partitions{{1, 7, 31, 64, 3, 127, 89, 78}};
    const auto blocked = render_partitioned(envelope, partitions);
    REQUIRE(blocked.size() == scalar.size());
    REQUIRE(blocked == scalar);

    envelope.trigger();
    const double first = envelope.next();
    for (int i = 0; i < 17; ++i)
        (void)envelope.next();
    envelope.reset();
    REQUIRE_FALSE(envelope.active());
    REQUIRE(envelope.current() == points[0].value);
    envelope.trigger();
    REQUIRE(envelope.next() == first);
}

TEST_CASE("Finite breakpoint loops repeat exactly then continue after the loop",
          "[signal][modulation-language][loop]") {
    BreakpointEnvelopeT<double, 4> envelope;
    REQUIRE(envelope.prepare(kSampleRate) == BreakpointEnvelopeStatus::ok);
    const std::array<BreakpointEnvelopePoint64, 4> points{{
        {0.0, 0.0, {}},
        {2.0, 1.0, {}},
        {4.0, 0.0, {}},
        {6.0, 2.0, {}},
    }};
    REQUIRE(envelope.configure(points) == BreakpointEnvelopeStatus::ok);
    REQUIRE(envelope.set_loop(1, 2, 1) == BreakpointEnvelopeStatus::ok);
    envelope.trigger();

    const std::array<double, 8> expected{{0.5, 1.0, 0.5, 0.0, 0.5, 0.0, 1.0, 2.0}};
    for (const auto value : expected)
        REQUIRE_THAT(envelope.next(), WithinAbs(value, 1.0e-12));
    REQUIRE(envelope.loops_completed() == 1);
    REQUIRE_FALSE(envelope.active());
}

TEST_CASE("Rise-fall is an exact continuous specialization of breakpoint playback",
          "[signal][modulation-language][rise-fall]") {
    RiseFallGenerator64 generator;
    REQUIRE(generator.prepare(kSampleRate) == BreakpointEnvelopeStatus::ok);
    REQUIRE(generator.set_levels(-1.0, 1.0));
    REQUIRE(generator.set_times_ms(2.0, 2.0));
    generator.set_looping(true);
    generator.trigger();

    REQUIRE(generator.stage() == RiseFallStage::rising);
    const std::array<double, 8> expected{{0.0, 1.0, 0.0, -1.0, 0.0, 1.0, 0.0, -1.0}};
    for (const auto value : expected)
        REQUIRE_THAT(generator.next(), WithinAbs(value, 1.0e-12));
    REQUIRE(generator.active());

    generator.set_looping(false);
    generator.reset();
    generator.trigger();
    for (int i = 0; i < 4; ++i)
        (void)generator.next();
    REQUIRE_FALSE(generator.active());
    REQUIRE(generator.stage() == RiseFallStage::idle);
    REQUIRE(generator.current() == -1.0);
}

TEST_CASE("Rise-fall setters reject nonfinite and overflowing programs transactionally",
          "[signal][modulation-language][fault]") {
    RiseFallGenerator64 generator;
    REQUIRE(generator.set_levels(-2.0, 3.0));
    REQUIRE_FALSE(generator.set_levels(std::numeric_limits<double>::infinity(), 1.0));
    REQUIRE(generator.low() == -2.0);
    REQUIRE(generator.high() == 3.0);

    REQUIRE(generator.set_times_ms(5.0, 7.0));
    REQUIRE_FALSE(generator.set_times_ms(-1.0, 2.0));
    REQUIRE_FALSE(generator.set_times_ms(RiseFallGenerator64::Engine::kMaxProgramTimeMs, 1.0));
    REQUIRE(generator.rise_ms() == 5.0);
    REQUIRE(generator.fall_ms() == 7.0);
}

TEST_CASE("Modulation playback and reset allocate no memory",
          "[signal][modulation-language][rt-safety]") {
    BreakpointEnvelope64 envelope;
    REQUIRE(envelope.prepare(192000.0) == BreakpointEnvelopeStatus::ok);
    const std::array<BreakpointEnvelopePoint64, 3> points{{
        {0.0, 0.0, {ModulationCurveShape::exponential, 1.0f}},
        {100.0, 1.0, {ModulationCurveShape::logarithmic, 1.0f}},
        {200.0, 0.0, {}},
    }};
    REQUIRE(envelope.configure(points) == BreakpointEnvelopeStatus::ok);
    REQUIRE(envelope.set_loop(0, 2, BreakpointEnvelope64::kLoopForever) ==
            BreakpointEnvelopeStatus::ok);
    std::array<double, 257> block{};

    pulp::test::RtAllocationProbe probe;
    envelope.trigger();
    for (int i = 0; i < 1000; ++i)
        envelope.process(block);
    envelope.reset();
    REQUIRE(probe.allocation_count() == 0);
}

// BeatRepeatKernel — exact history, tempo quantization, and bounded gestures.

#include <pulp/signal/beat_repeat_kernel.hpp>
#include <pulp/signal/freeze_loop_sampler.hpp>
#include <pulp/timebase/beat_division.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace pulp::signal;
using namespace pulp::timebase;

namespace {

constexpr RationalRate kSampleRate{480, 1};
constexpr std::int64_t kQuarterFrames = 240; // 120 BPM at 480 Hz.

CompiledTempoMap constant_map(double bpm = 120.0, RationalRate sample_rate = kSampleRate) {
    const std::array points{TempoPoint{{0}, bpm, TempoCurve::Constant}};
    auto compiled = CompiledTempoMap::compile(points, sample_rate);
    REQUIRE(compiled);
    return std::move(compiled).value();
}

CompiledTempoMap ramp_map() {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{4 * kTicksPerQuarter}, 180.0, TempoCurve::Constant},
    };
    auto compiled = CompiledTempoMap::compile(points, kSampleRate);
    REQUIRE(compiled);
    return std::move(compiled).value();
}

BeatRepeatKernel prepared_kernel(std::size_t history = 4096, std::size_t maximum_transition = 64) {
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare(kSampleRate, 1, history, maximum_transition));
    kernel.set_transition_samples(0);
    return kernel;
}

BeatRepeatEvent trigger(std::uint32_t offset = 0) {
    return {BeatRepeatEvent::Type::Trigger, offset, 0};
}

BeatRepeatEvent stop(std::uint32_t offset = 0) {
    return {BeatRepeatEvent::Type::Stop, offset, 0};
}

BeatRepeatEvent seek(std::uint32_t offset, std::int64_t captured_frame) {
    return {BeatRepeatEvent::Type::Seek, offset, captured_frame};
}

struct ScheduledEvent {
    std::int64_t sample = 0;
    BeatRepeatEvent event{};
};

struct RenderResult {
    std::vector<float> output;
    BeatRepeatState state = BeatRepeatState::Idle;
    BeatRepeatError error = BeatRepeatError::None;
    std::size_t rejected_events = 0;
};

std::vector<float> ramp_input(std::size_t frames, float offset = 0.0f) {
    std::vector<float> input(frames);
    for (std::size_t i = 0; i < frames; ++i)
        input[i] = offset + static_cast<float>(i);
    return input;
}

std::vector<int> fixed_partitions(std::size_t total, int block) {
    std::vector<int> result;
    while (total != 0) {
        const int next = std::min<int>(block, static_cast<int>(total));
        result.push_back(next);
        total -= static_cast<std::size_t>(next);
    }
    return result;
}

RenderResult render(BeatRepeatKernel& kernel, const CompiledTempoMap& tempo,
                    std::span<const float> input, std::span<const int> partitions,
                    std::span<const ScheduledEvent> schedule = {}, std::uint64_t epoch = 1,
                    std::int64_t start_sample = 0) {
    RenderResult rendered;
    rendered.output.resize(input.size(), -999.0f);
    std::size_t cursor = 0;

    for (const int requested : partitions) {
        if (cursor == input.size())
            break;
        const auto frames =
            std::min<std::size_t>(static_cast<std::size_t>(requested), input.size() - cursor);
        const auto block_start = start_sample + static_cast<std::int64_t>(cursor);
        const auto block_end = block_start + static_cast<std::int64_t>(frames);
        std::vector<BeatRepeatEvent> events;
        for (const auto& scheduled : schedule) {
            if (scheduled.sample >= block_start && scheduled.sample < block_end) {
                auto event = scheduled.event;
                event.frame_offset = static_cast<std::uint32_t>(scheduled.sample - block_start);
                events.push_back(event);
            }
        }
        std::sort(events.begin(), events.end(), [](const auto& left, const auto& right) {
            return left.frame_offset < right.frame_offset;
        });

        const float* in[]{input.data() + cursor};
        float* out[]{rendered.output.data() + cursor};
        const auto result = kernel.process(in, out, frames, block_start, epoch, tempo, events);
        rendered.error = result.error;
        rendered.rejected_events += result.rejected_events;
        REQUIRE(result.processed_frames == frames);
        cursor += frames;
    }

    REQUIRE(cursor == input.size());
    rendered.state = kernel.state();
    return rendered;
}

BeatRepeatProcessResult process_segment(BeatRepeatKernel& kernel, const CompiledTempoMap& tempo,
                                        std::span<const float> input, std::span<float> output,
                                        std::int64_t start_sample, std::uint64_t epoch,
                                        std::span<const BeatRepeatEvent> events = {}) {
    REQUIRE(input.size() == output.size());
    const float* in[]{input.data()};
    float* out[]{output.data()};
    return kernel.process(in, out, input.size(), start_sample, epoch, tempo, events);
}

void require_span_equals(std::span<const float> actual, std::span<const float> expected) {
    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
        REQUIRE_THAT(actual[i], WithinAbs(expected[i], 1.0e-6f));
}

} // namespace

TEST_CASE("FreezeLoopSampler exact capture is non-clamping and transactional",
          "[signal][beat-repeat][history]") {
    FreezeLoopSampler sampler;
    sampler.prepare(1, 8, 0);
    REQUIRE(sampler.capacity() == 8);
    REQUIRE(sampler.available_history() == 0);

    const std::array first{0.0f, 1.0f, 2.0f, 3.0f, 4.0f};
    const float* first_in[]{first.data()};
    sampler.write(first_in, static_cast<int>(first.size()));
    REQUIRE(sampler.available_history() == 5);
    REQUIRE_FALSE(sampler.capture_recent_exact(6));
    REQUIRE(sampler.captured_channel(0).empty());

    REQUIRE(sampler.capture_recent_exact(4));
    const std::array expected_first{1.0f, 2.0f, 3.0f, 4.0f};
    require_span_equals(sampler.captured_channel(0), expected_first);

    const std::array wrapped{5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f};
    const float* wrapped_in[]{wrapped.data()};
    sampler.write(wrapped_in, static_cast<int>(wrapped.size()));
    REQUIRE(sampler.capture_recent_exact(8));
    const std::array expected_wrapped{3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f};
    require_span_equals(sampler.captured_channel(0), expected_wrapped);
    REQUIRE_FALSE(sampler.capture_recent_exact(9));
    require_span_equals(sampler.captured_channel(0), expected_wrapped);
    REQUIRE(sampler.retained_bytes() >= 2 * 8 * sizeof(float));

    FreezeLoopSampler malformed;
    malformed.prepare(1, 8, 0);
    REQUIRE_FALSE(malformed.restore({std::numeric_limits<float>::quiet_NaN(), 4.0f, 0.0f, 0.0f}));
    REQUIRE_FALSE(malformed.restore({1.0f, std::numeric_limits<float>::infinity(), 0.0f, 0.0f}));
    REQUIRE_FALSE(malformed.restore({1.0f, 4.0f, 0.0f, -1.0f, 1.0f, 2.0f, 3.0f, 4.0f}));
    REQUIRE_FALSE(malformed.restore(
        {1.0f, 4.0f, 0.0f, 0.0f, 1.0f, std::numeric_limits<float>::quiet_NaN(), 3.0f, 4.0f}));
    REQUIRE_FALSE(
        malformed.restore({1.0f, 1.0f, 0.0f, 0.0f, 1.0f, std::numeric_limits<float>::quiet_NaN()}));

    FreezeLoopSampler invalid;
    invalid.prepare(-1, 8, 0);
    REQUIRE(invalid.channels() == 0);
    REQUIRE(invalid.capacity() == 0);
    invalid.prepare(1, 8, std::numeric_limits<int>::max());
    REQUIRE(invalid.capacity() == 0);
    invalid.prepare(1, 16'777'217, 0);
    REQUIRE(invalid.capacity() == 0);
    invalid.freeze(1);
    REQUIRE_FALSE(invalid.frozen());
}

TEST_CASE("beat repeat uses the strictly next edge and captures before its sample",
          "[signal][beat-repeat][timing]") {
    const auto tempo = constant_map();
    auto kernel = prepared_kernel();
    kernel.set_division(BeatDivision::Quarter);
    const auto input = ramp_input(3 * kQuarterFrames);
    const std::array events{ScheduledEvent{kQuarterFrames, trigger()}};
    const auto result = render(kernel, tempo, input, fixed_partitions(input.size(), 73), events);

    REQUIRE(result.output[kQuarterFrames] == input[kQuarterFrames]);
    REQUIRE(result.output[2 * kQuarterFrames - 1] == input[2 * kQuarterFrames - 1]);
    REQUIRE(result.output[2 * kQuarterFrames] == input[kQuarterFrames]);
    REQUIRE(result.output[2 * kQuarterFrames + 1] == input[kQuarterFrames + 1]);
    REQUIRE(kernel.captured_frames() == kQuarterFrames);
    REQUIRE(result.state == BeatRepeatState::Active);
}

TEST_CASE("beat repeat resolves strict next edges through tempo ramps",
          "[signal][beat-repeat][tempo-ramp]") {
    const auto tempo = ramp_map();
    const auto ticks = division_ticks(BeatDivision::Eighth);
    REQUIRE(ticks);
    const auto first_edge = tempo.ticks_to_samples({ticks.value().value}).value;
    const auto second_edge = tempo.ticks_to_samples({2 * ticks.value().value}).value;
    const auto captured = second_edge - first_edge;

    auto kernel = prepared_kernel();
    kernel.set_division(BeatDivision::Eighth);
    const auto input = ramp_input(static_cast<std::size_t>(second_edge + captured + 1), 1000.0f);
    const std::array events{ScheduledEvent{first_edge, trigger()}};
    const auto result = render(kernel, tempo, input, fixed_partitions(input.size(), 37), events);

    REQUIRE(result.output[first_edge] == input[first_edge]);
    REQUIRE(result.output[second_edge] == input[first_edge]);
    REQUIRE(result.output[second_edge + captured - 1] == input[second_edge - 1]);
    REQUIRE(kernel.captured_frames() == static_cast<std::size_t>(captured));
}

TEST_CASE("strict next edge does not skip a nearby positive or negative edge",
          "[signal][beat-repeat][timing][sparse-grid]") {
    constexpr RationalRate rate{48'000, 1};
    const auto tempo = constant_map(120.0, rate);
    constexpr std::size_t quarter = 24'000;

    SECTION("one sample before a positive edge") {
        BeatRepeatKernel kernel;
        REQUIRE(kernel.prepare(rate, 1, 30'000, 0));
        kernel.set_division(BeatDivision::Quarter);
        const auto input = ramp_input(quarter + 2);
        std::vector<float> output(input.size());
        const std::array event{trigger(static_cast<std::uint32_t>(quarter - 1))};
        REQUIRE(process_segment(kernel, tempo, input, output, 0, 1, event));
        REQUIRE(output[quarter - 1] == input[quarter - 1]);
        REQUIRE(output[quarter] == input[0]);
    }

    SECTION("one sample before transport tick zero") {
        BeatRepeatKernel kernel;
        REQUIRE(kernel.prepare(rate, 1, 30'000, 0));
        kernel.set_division(BeatDivision::Quarter);
        const auto input = ramp_input(quarter + 2, 1000.0f);
        std::vector<float> output(input.size());
        const std::array event{trigger(static_cast<std::uint32_t>(quarter))};
        REQUIRE(process_segment(kernel, tempo, input, output,
                                -static_cast<std::int64_t>(quarter) - 1, 1, event));
        REQUIRE(output[quarter + 1] == input[1]);
    }

    SECTION("sample grid is denser than the canonical tick lattice") {
        constexpr RationalRate sparse_tick_rate{768'000, 1};
        constexpr std::size_t sparse_cell = 1'920'000;
        const auto sparse_tempo = constant_map(1.0, sparse_tick_rate);
        BeatRepeatKernel kernel;
        REQUIRE(kernel.prepare(sparse_tick_rate, 1, sparse_cell + 16, 0));
        kernel.set_division(BeatDivision::SixtyFourthTriplet);
        const auto input = ramp_input(sparse_cell + 2);
        std::vector<float> output(input.size());
        const std::array event{trigger(static_cast<std::uint32_t>(sparse_cell - 1))};
        REQUIRE(process_segment(kernel, sparse_tempo, input, output, 0, 1, event));
        REQUIRE(output[sparse_cell] == input[0]);
    }
}

TEST_CASE("beat repeat is invariant to process block partitioning",
          "[signal][beat-repeat][partition]") {
    const auto tempo = ramp_map();
    const auto input = ramp_input(1800, -500.0f);
    const std::array events{
        ScheduledEvent{113, trigger()},
        ScheduledEvent{947, seek(0, 11)},
        ScheduledEvent{1311, stop()},
    };

    auto one = prepared_kernel();
    auto many = prepared_kernel();
    one.set_division(BeatDivision::Eighth);
    many.set_division(BeatDivision::Eighth);
    one.set_direction(BeatRepeatDirection::Alternate);
    many.set_direction(BeatRepeatDirection::Alternate);
    const std::array one_block{1800};
    const std::array odd_blocks{1, 7, 31, 2, 127, 19, 3, 251, 43, 509, 29, 64, 128, 256, 330};
    const auto a = render(one, tempo, input, one_block, events);
    const auto b = render(many, tempo, input, odd_blocks, events);
    require_span_equals(a.output, b.output);
    REQUIRE(a.state == b.state);
    REQUIRE(a.error == b.error);
    REQUIRE(a.rejected_events == b.rejected_events);
}

TEST_CASE("insufficient-history retrigger preserves the active loop",
          "[signal][beat-repeat][transactional]") {
    const auto tempo = constant_map();
    auto reference = prepared_kernel(300);
    auto retriggered = prepared_kernel(300);
    reference.set_division(BeatDivision::Quarter);
    retriggered.set_division(BeatDivision::Quarter);
    const auto input = ramp_input(1500);
    std::vector<float> expected(input.size());
    std::vector<float> actual(input.size());
    const std::array first_trigger{trigger(1)};

    REQUIRE(process_segment(reference, tempo, std::span(input).first(721),
                            std::span(expected).first(721), 0, 1, first_trigger));
    REQUIRE(process_segment(retriggered, tempo, std::span(input).first(721),
                            std::span(actual).first(721), 0, 1, first_trigger));
    retriggered.set_division(BeatDivision::Whole);
    const std::array failed_trigger{trigger(0)};
    const auto expected_tail = process_segment(reference, tempo, std::span(input).subspan(721),
                                               std::span(expected).subspan(721), 721, 1);
    const auto actual_tail =
        process_segment(retriggered, tempo, std::span(input).subspan(721),
                        std::span(actual).subspan(721), 721, 1, failed_trigger);

    REQUIRE(expected_tail.processed_frames == input.size() - 721);
    REQUIRE(actual_tail.processed_frames == input.size() - 721);
    require_span_equals(actual, expected);
    REQUIRE(retriggered.state() == BeatRepeatState::Active);
    REQUIRE(retriggered.last_capture_rejected());
    REQUIRE(actual_tail.error == BeatRepeatError::InsufficientHistory);
}

TEST_CASE("beat repeat retrigger history contains dry input rather than wet output",
          "[signal][beat-repeat][history][dry-only]") {
    const auto tempo = constant_map();
    auto kernel = prepared_kernel();
    kernel.set_division(BeatDivision::Quarter);
    std::vector<float> input(1200);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = i < 480 ? static_cast<float>(i) : 10'000.0f + static_cast<float>(i);
    const std::array events{
        ScheduledEvent{1, trigger()},
        ScheduledEvent{481, trigger()},
    };
    const auto result = render(kernel, tempo, input, fixed_partitions(input.size(), 53), events);

    REQUIRE(result.output[720] == input[480]);
    REQUIRE(result.output[720] > 10'000.0f);
    REQUIRE_FALSE(kernel.last_capture_rejected());
}

TEST_CASE("beat repeat preserves dry history when input aliases output",
          "[signal][beat-repeat][history][in-place]") {
    const auto tempo = constant_map();
    auto reference = prepared_kernel();
    auto in_place = prepared_kernel();
    reference.set_division(BeatDivision::Quarter);
    in_place.set_division(BeatDivision::Quarter);
    auto dry = ramp_input(1200, 10'000.0f);
    auto aliased = dry;
    std::vector<float> expected(dry.size());
    const std::array events{trigger(1), trigger(481)};

    REQUIRE(process_segment(reference, tempo, dry, expected, 0, 1, events));
    const float* input[]{aliased.data()};
    float* output[]{aliased.data()};
    REQUIRE(in_place.process(input, output, aliased.size(), 0, 1, tempo, events));

    require_span_equals(aliased, expected);
    REQUIRE(aliased[720] == dry[480]);
}

TEST_CASE("forward reverse and alternate use exact captured-frame indexing",
          "[signal][beat-repeat][direction]") {
    const auto tempo = constant_map();
    const auto input = ramp_input(4 * kQuarterFrames);
    const auto blocks = fixed_partitions(input.size(), 64);

    auto run = [&](BeatRepeatDirection direction) {
        auto kernel = prepared_kernel();
        kernel.set_division(BeatDivision::Quarter);
        kernel.set_direction(direction);
        const std::array events{ScheduledEvent{1, trigger()}};
        return render(kernel, tempo, input, blocks, events).output;
    };

    const auto forward = run(BeatRepeatDirection::Forward);
    const auto reverse = run(BeatRepeatDirection::Reverse);
    const auto alternate = run(BeatRepeatDirection::Alternate);
    REQUIRE(forward[kQuarterFrames] == input[0]);
    REQUIRE(forward[2 * kQuarterFrames - 1] == input[kQuarterFrames - 1]);
    REQUIRE(reverse[kQuarterFrames] == input[kQuarterFrames - 1]);
    REQUIRE(reverse[2 * kQuarterFrames - 1] == input[0]);
    REQUIRE(alternate[kQuarterFrames] == input[0]);
    REQUIRE(alternate[2 * kQuarterFrames] == input[kQuarterFrames - 1]);
    REQUIRE(alternate[2 * kQuarterFrames + 1] == input[kQuarterFrames - 2]);
}

TEST_CASE("repeat count includes the first cell and gate is a bounded duty cycle",
          "[signal][beat-repeat][repeat-count][gate]") {
    const auto tempo = constant_map();
    const auto input = ramp_input(4 * kQuarterFrames, 5000.0f);

    SECTION("finite repeat count returns to dry") {
        auto kernel = prepared_kernel();
        kernel.set_division(BeatDivision::Quarter);
        kernel.set_repeat_count(2);
        const std::array events{ScheduledEvent{1, trigger()}};
        const auto result =
            render(kernel, tempo, input, fixed_partitions(input.size(), 41), events);
        REQUIRE(result.output[kQuarterFrames] == input[0]);
        REQUIRE(result.output[2 * kQuarterFrames] == input[0]);
        REQUIRE(result.output[3 * kQuarterFrames] == input[3 * kQuarterFrames]);
        REQUIRE(result.state == BeatRepeatState::Idle);
    }

    SECTION("half gate silences the second half of each repeated cell") {
        auto kernel = prepared_kernel();
        kernel.set_division(BeatDivision::Quarter);
        kernel.set_gate(0.5f);
        const std::array events{ScheduledEvent{1, trigger()}};
        const auto result =
            render(kernel, tempo, input, fixed_partitions(input.size(), 41), events);
        REQUIRE(result.output[kQuarterFrames] == input[0]);
        REQUIRE(result.output[kQuarterFrames + 119] == input[119]);
        REQUIRE(result.output[kQuarterFrames + 120] == 0.0f);
        REQUIRE(result.output[2 * kQuarterFrames - 1] == 0.0f);
    }
}

TEST_CASE("stop seek and retrigger honor frame offsets", "[signal][beat-repeat][events]") {
    const auto tempo = constant_map();
    const auto input = ramp_input(1100, 20'000.0f);
    auto kernel = prepared_kernel();
    kernel.set_division(BeatDivision::Quarter);
    const std::array events{
        ScheduledEvent{1, trigger()},
        ScheduledEvent{300, seek(0, 10)},
        ScheduledEvent{350, stop()},
        ScheduledEvent{401, trigger()},
    };
    const auto result = render(kernel, tempo, input, fixed_partitions(input.size(), 128), events);

    REQUIRE(result.output[299] == input[59]);
    REQUIRE(result.output[300] == input[10]);
    REQUIRE(result.output[349] == input[59]);
    REQUIRE(result.output[350] == input[350]);
    REQUIRE(result.output[479] == input[479]);
    REQUIRE(result.output[480] == input[240]);
}

TEST_CASE("same-frame events execute in caller span order",
          "[signal][beat-repeat][events][ordering]") {
    const auto tempo = constant_map();
    auto make_active = [&] {
        BeatRepeatKernel kernel;
        REQUIRE(kernel.prepare(kSampleRate, 1, 4096, 8));
        kernel.set_division(BeatDivision::Quarter);
        kernel.set_transition_samples(8);
        const auto input = ramp_input(300);
        std::vector<float> output(input.size());
        const std::array arm{trigger(1)};
        REQUIRE(process_segment(kernel, tempo, input, output, 0, 1, arm));
        return kernel;
    };

    const std::array<float, 1> input{300.0f};
    std::array<float, 1> output{};
    auto stop_then_trigger = make_active();
    const std::array first{stop(0), trigger(0)};
    REQUIRE(process_segment(stop_then_trigger, tempo, input, output, 300, 1, first));
    REQUIRE(stop_then_trigger.state() == BeatRepeatState::ReleasingArmed);

    auto trigger_then_stop = make_active();
    const std::array second{trigger(0), stop(0)};
    REQUIRE(process_segment(trigger_then_stop, tempo, input, output, 300, 1, second));
    REQUIRE(trigger_then_stop.state() == BeatRepeatState::Releasing);
}

TEST_CASE("transport epoch changes cancel pending arms but preserve active capture",
          "[signal][beat-repeat][epoch]") {
    const auto tempo = constant_map();

    SECTION("pending arm is cancelled at the next process boundary") {
        auto kernel = prepared_kernel();
        kernel.set_division(BeatDivision::Quarter);
        const auto input = ramp_input(500);
        std::vector<float> output(input.size());
        const std::array arm{trigger(10)};
        REQUIRE(process_segment(kernel, tempo, std::span(input).first(100),
                                std::span(output).first(100), 0, 1, arm));
        REQUIRE(kernel.state() == BeatRepeatState::Armed);
        REQUIRE(process_segment(kernel, tempo, std::span(input).subspan(100),
                                std::span(output).subspan(100), 100, 2));
        require_span_equals(output, input);
        REQUIRE(kernel.state() == BeatRepeatState::Idle);
    }

    SECTION("active loop survives and new history starts at the epoch") {
        auto kernel = prepared_kernel();
        kernel.set_division(BeatDivision::Eighth);
        const auto input = ramp_input(700);
        std::vector<float> output(input.size());
        const std::array first_trigger{trigger(1)};
        REQUIRE(process_segment(kernel, tempo, std::span(input).first(300),
                                std::span(output).first(300), 0, 1, first_trigger));
        const std::array new_trigger{trigger(10)};
        const auto second = process_segment(kernel, tempo, std::span(input).subspan(300),
                                            std::span(output).subspan(300), 300, 2, new_trigger);
        REQUIRE(second.processed_frames == input.size() - 300);
        REQUIRE(second.error == BeatRepeatError::InsufficientHistory);
        REQUIRE(output[300] == input[60]);
        REQUIRE(output[360] == input[0]);
        REQUIRE(kernel.state() == BeatRepeatState::Active);
        REQUIRE(kernel.last_capture_rejected());
    }
}

TEST_CASE("nonzero transitions smooth finite release and reject transient snapshots",
          "[signal][beat-repeat][transition][snapshot]") {
    const auto tempo = constant_map();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare(kSampleRate, 1, 4096, 8));
    kernel.set_division(BeatDivision::Quarter);
    kernel.set_repeat_count(1);
    kernel.set_transition_samples(8);
    const auto input = ramp_input(600);
    std::vector<float> output(input.size());
    const std::array event{trigger(1)};
    REQUIRE(process_segment(kernel, tempo, input, output, 0, 1, event));

    // The final wet sample is 239. Release begins at sample 480 from that
    // held endpoint, rather than jumping back to capture frame zero.
    REQUIRE(output[479] == 239.0f);
    REQUIRE(output[480] > output[479]);
    REQUIRE(output[480] < input[480]);
    REQUIRE(std::abs(output[480] - output[479]) < 50.0f);
    REQUIRE(output[488] == input[488]);
    REQUIRE(std::abs(output[240] - output[239]) < 50.0f);

    BeatRepeatKernel wrapping;
    REQUIRE(wrapping.prepare(kSampleRate, 1, 4096, 8));
    wrapping.set_division(BeatDivision::Quarter);
    wrapping.set_transition_samples(8);
    std::vector<float> wrap_output(input.size());
    REQUIRE(process_segment(wrapping, tempo, input, wrap_output, 0, 1, event));
    REQUIRE(std::abs(wrap_output[480] - wrap_output[479]) < 50.0f);

    BeatRepeatKernel gated;
    REQUIRE(gated.prepare(kSampleRate, 1, 4096, 8));
    gated.set_division(BeatDivision::Quarter);
    gated.set_transition_samples(8);
    gated.set_gate(0.5f);
    std::vector<float> gate_output(input.size());
    REQUIRE(process_segment(gated, tempo, input, gate_output, 0, 1, event));
    REQUIRE(std::abs(gate_output[360] - gate_output[359]) < 50.0f);

    BeatRepeatKernel seeking;
    REQUIRE(seeking.prepare(kSampleRate, 1, 4096, 8));
    seeking.set_division(BeatDivision::Quarter);
    seeking.set_transition_samples(8);
    std::vector<float> seek_output(input.size());
    const std::array seek_events{trigger(1), seek(300, 10)};
    REQUIRE(process_segment(seeking, tempo, input, seek_output, 0, 1, seek_events));
    REQUIRE(std::abs(seek_output[300] - seek_output[299]) < 50.0f);

    BeatRepeatKernel transitioning;
    REQUIRE(transitioning.prepare(kSampleRate, 1, 4096, 8));
    transitioning.set_division(BeatDivision::Quarter);
    transitioning.set_transition_samples(8);
    std::array<float, 241> short_input{};
    std::array<float, 241> short_output{};
    const std::array short_event{trigger(1)};
    REQUIRE(process_segment(transitioning, tempo, short_input, short_output, 0, 1, short_event));
    const auto transient = transitioning.snapshot();
    REQUIRE_FALSE(transient.restorable);
    BeatRepeatKernel destination;
    REQUIRE(destination.prepare(kSampleRate, 1, 4096, 8));
    REQUIRE_FALSE(destination.restore(transient));

    SECTION("stop during dry-to-wet transition starts from the audible sample") {
        BeatRepeatKernel overlapping;
        REQUIRE(overlapping.prepare(kSampleRate, 1, 4096, 8));
        overlapping.set_division(BeatDivision::Quarter);
        overlapping.set_transition_samples(8);
        const auto overlap_input = ramp_input(260);
        std::vector<float> overlap_output(overlap_input.size());
        const std::array overlap_events{trigger(1), stop(241)};
        REQUIRE(process_segment(overlapping, tempo, overlap_input, overlap_output, 0, 1,
                                overlap_events));
        REQUIRE(std::abs(overlap_output[241] - overlap_output[240]) < 50.0f);
    }

    SECTION("seek during dry-to-wet transition starts from the audible sample") {
        BeatRepeatKernel overlapping;
        REQUIRE(overlapping.prepare(kSampleRate, 1, 4096, 8));
        overlapping.set_division(BeatDivision::Quarter);
        overlapping.set_transition_samples(8);
        const auto overlap_input = ramp_input(260);
        std::vector<float> overlap_output(overlap_input.size());
        const std::array overlap_events{trigger(1), seek(241, 100)};
        REQUIRE(process_segment(overlapping, tempo, overlap_input, overlap_output, 0, 1,
                                overlap_events));
        REQUIRE(std::abs(overlap_output[241] - overlap_output[240]) < 50.0f);
    }

    SECTION("seek during release cancels release without wedging state") {
        BeatRepeatKernel overlapping;
        REQUIRE(overlapping.prepare(kSampleRate, 1, 4096, 8));
        overlapping.set_division(BeatDivision::Quarter);
        overlapping.set_transition_samples(8);
        const auto overlap_input = ramp_input(280);
        std::vector<float> overlap_output(overlap_input.size());
        const std::array overlap_events{trigger(1), stop(250), seek(251, 100)};
        REQUIRE(process_segment(overlapping, tempo, overlap_input, overlap_output, 0, 1,
                                overlap_events));
        REQUIRE(overlapping.state() == BeatRepeatState::Active);
        REQUIRE(std::abs(overlap_output[251] - overlap_output[250]) < 50.0f);
    }
}

TEST_CASE("beat repeat has zero latency and a transition-bounded tail",
          "[signal][beat-repeat][latency][tail]") {
    auto kernel = prepared_kernel(4096, 64);
    REQUIRE(kernel.latency_samples() == 0);
    REQUIRE(kernel.max_tail_samples() == 0);
    kernel.set_transition_samples(64);
    REQUIRE(kernel.max_tail_samples() == 64);
    kernel.set_transition_samples(1000);
    REQUIRE(kernel.transition_samples() == 64);
    REQUIRE(kernel.max_tail_samples() == 64);
}

TEST_CASE("beat repeat reports nonfinite, range, tempo, and event errors",
          "[signal][beat-repeat][errors]") {
    SECTION("prepare and setters reject invalid values") {
        BeatRepeatKernel kernel;
        REQUIRE_FALSE(kernel.prepare(kSampleRate, 0, 0, 0));
        REQUIRE_FALSE(kernel.prepare(kSampleRate, 64,
                                     static_cast<std::size_t>(std::numeric_limits<int>::max()), 0));
        REQUIRE(kernel.prepare(kSampleRate, 1, 128, 16));
        REQUIRE(kernel.prepare(kSampleRate, 1, 256, 8));
        REQUIRE(kernel.state() == BeatRepeatState::Idle);
        REQUIRE(kernel.retained_bytes() == (2 * 256 + 5) * sizeof(float));
        kernel.set_division(static_cast<BeatDivision>(255));
        REQUIRE(kernel.last_error() == BeatRepeatError::InvalidArgument);
        kernel.clear_error();
        kernel.set_direction(static_cast<BeatRepeatDirection>(255));
        REQUIRE(kernel.last_error() == BeatRepeatError::InvalidArgument);
        kernel.clear_error();
        kernel.set_gate(std::numeric_limits<float>::quiet_NaN());
        REQUIRE(kernel.last_error() == BeatRepeatError::InvalidArgument);
    }

    SECTION("nonfinite dry input is sanitized") {
        const auto tempo = constant_map();
        auto kernel = prepared_kernel();
        auto input = ramp_input(32);
        input[7] = std::numeric_limits<float>::quiet_NaN();
        input[8] = std::numeric_limits<float>::infinity();
        std::array<int, 1> blocks{32};
        const auto result = render(kernel, tempo, input, blocks);
        REQUIRE(
            std::ranges::all_of(result.output, [](float value) { return std::isfinite(value); }));
        REQUIRE(result.output[7] == 0.0f);
        REQUIRE(result.output[8] == 0.0f);
        REQUIRE(result.error == BeatRepeatError::NonFiniteInput);

        auto in_place = prepared_kernel();
        std::array<float, 2> aliased{std::numeric_limits<float>::quiet_NaN(), 1.0f};
        const float* alias_input[]{aliased.data()};
        float* alias_output[]{aliased.data()};
        const auto alias_result =
            in_place.process(alias_input, alias_output, aliased.size(), 0, 1, tempo);
        REQUIRE(alias_result.error == BeatRepeatError::NonFiniteInput);
        REQUIRE(aliased[0] == 0.0f);
        REQUIRE(aliased[1] == 1.0f);
    }

    SECTION("tempo rate mismatch and position overflow fail before processing") {
        auto kernel = prepared_kernel();
        const auto wrong_rate = constant_map(120.0, RationalRate{960, 1});
        const std::array<float, 2> input{1.0f, 2.0f};
        std::array<float, 2> output{};
        auto mismatch = process_segment(kernel, wrong_rate, input, output, 0, 1);
        REQUIRE(mismatch.error == BeatRepeatError::TempoMapRateMismatch);
        REQUIRE(mismatch.processed_frames == 0);
        kernel.clear_error();
        const auto tempo = constant_map();
        auto overflow = process_segment(kernel, tempo, input, output,
                                        std::numeric_limits<std::int64_t>::max() - 1, 1);
        REQUIRE(overflow.error == BeatRepeatError::PositionOverflow);
        REQUIRE(overflow.processed_frames == 0);
    }

    SECTION("out-of-range and unordered events are rejected") {
        const auto tempo = constant_map();
        auto kernel = prepared_kernel();
        const std::array<float, 16> input{};
        std::array<float, 16> output{};
        const std::array events{trigger(12), stop(3), seek(16, 0)};
        const auto result = process_segment(kernel, tempo, input, output, 0, 1, events);
        REQUIRE(result.processed_frames == input.size());
        REQUIRE(result.rejected_events == 2);
        REQUIRE(result.error == BeatRepeatError::InvalidEventOrder);
    }
}

TEST_CASE("beat repeat snapshot restores active capture and playhead",
          "[signal][beat-repeat][snapshot]") {
    const auto tempo = constant_map();
    auto original = prepared_kernel();
    original.set_division(BeatDivision::Quarter);
    original.set_direction(BeatRepeatDirection::Alternate);
    original.set_repeat_count(7);
    original.set_gate(0.75f);
    original.set_transition_samples(8);
    const auto prefix = ramp_input(377);
    std::vector<float> prefix_output(prefix.size());
    const std::array arm{trigger(1)};
    REQUIRE(process_segment(original, tempo, prefix, prefix_output, 0, 1, arm));

    const auto snapshot = original.snapshot();
    REQUIRE(snapshot.restorable);
    REQUIRE(snapshot.active);
    REQUIRE_FALSE(snapshot.capture.empty());
    auto restored = prepared_kernel();
    REQUIRE(restored.restore(snapshot));
    REQUIRE(restored.direction() == BeatRepeatDirection::Alternate);
    REQUIRE(restored.repeat_count() == 7);
    REQUIRE(restored.gate() == 0.75f);

    const auto suffix = ramp_input(400, 50'000.0f);
    std::vector<float> expected(suffix.size());
    std::vector<float> actual(suffix.size());
    const std::array immediate_stop{stop(0)};
    REQUIRE(process_segment(original, tempo, suffix, expected, 377, 1, immediate_stop));
    REQUIRE(process_segment(restored, tempo, suffix, actual, 377, 1, immediate_stop));
    require_span_equals(actual, expected);

    auto malformed = snapshot;
    malformed.version = 2;
    auto rejected = prepared_kernel();
    REQUIRE_FALSE(rejected.restore(malformed));
    REQUIRE(rejected.state() == BeatRepeatState::Idle);

    malformed = snapshot;
    malformed.last_wet[0] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_FALSE(rejected.restore(malformed));
    malformed = snapshot;
    malformed.last_rendered[0] = std::numeric_limits<float>::infinity();
    REQUIRE_FALSE(rejected.restore(malformed));
    malformed = snapshot;
    malformed.active = false;
    REQUIRE_FALSE(rejected.restore(malformed));
}

TEST_CASE("beat repeat process and event handling allocate nothing after prepare",
          "[signal][beat-repeat][rt]") {
    const auto tempo = constant_map();
    auto kernel = prepared_kernel();
    kernel.set_division(BeatDivision::Sixteenth);
    kernel.set_direction(BeatRepeatDirection::Alternate);
    std::array<float, 128> input{};
    std::array<float, 128> output{};
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i);
    const float* in[]{input.data()};
    float* out[]{output.data()};
    const std::array events{trigger(7), seek(91, 3)};

    std::size_t allocations = 1;
    BeatRepeatProcessResult result;
    {
        pulp::test::RtAllocationProbe probe;
        result = kernel.process(in, out, input.size(), 0, 1, tempo, events);
        const std::array stop_event{stop(0)};
        result = kernel.process(in, out, input.size(), 128, 1, tempo, stop_event);
        kernel.reset();
        allocations = probe.allocation_count();
    }
    REQUIRE(result.processed_frames == input.size());
    REQUIRE(result.rejected_events == 0);
    REQUIRE(allocations == 0);
    REQUIRE(kernel.retained_bytes() == (2 * 4096 + 5) * sizeof(float));
}

#include <pulp/signal/beat_repeat_kernel.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace pulp::signal;
using namespace pulp::timebase;

namespace {

CompiledTempoMap tempo_map(std::span<const TempoPoint> points, std::uint64_t sample_rate = 1'000) {
    auto compiled = CompiledTempoMap::compile(points, {sample_rate, 1});
    REQUIRE(compiled);
    return std::move(compiled.value());
}

CompiledTempoMap constant_tempo(double bpm = 120.0, std::int64_t sample_rate = 1'000) {
    const std::array points{TempoPoint{{0}, bpm, TempoCurve::Constant}};
    return tempo_map(points, sample_rate);
}

std::vector<float> ramp(int frames, float scale = 0.001f) {
    std::vector<float> result(static_cast<std::size_t>(frames));
    for (int frame = 0; frame < frames; ++frame)
        result[static_cast<std::size_t>(frame)] = static_cast<float>(frame) * scale;
    return result;
}

std::vector<float> render_partitioned(const CompiledTempoMap& tempo,
                                      std::span<const int> partitions, int total_frames,
                                      int transition_frames = 8) {
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, transition_frames, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(2));
    const auto armed = kernel.trigger(tempo, BeatDivision::Eighth, {510});
    REQUIRE(armed);

    auto input = ramp(total_frames);
    std::vector<float> output(static_cast<std::size_t>(total_frames), -99.0f);
    int offset = 0;
    std::size_t partition = 0;
    while (offset < total_frames) {
        const int requested = partitions[partition++ % partitions.size()];
        const int frames = std::min(requested, total_frames - offset);
        const float* inputs[]{input.data() + offset};
        float* outputs[]{output.data() + offset};
        kernel.process(inputs, outputs, frames, {offset});
        offset += frames;
    }
    return output;
}

float maximum_step(std::span<const float> values, int begin, int end) {
    float result = 0.0f;
    for (int frame = std::max(1, begin); frame < std::min<int>(end, values.size()); ++frame)
        result = std::max(result, std::abs(values[static_cast<std::size_t>(frame)] -
                                           values[static_cast<std::size_t>(frame - 1)]));
    return result;
}

template <typename Function> void require_allocates_no_memory(Function&& function) {
    pulp::test::RtAllocationProbe probe;
    function();
    REQUIRE(probe.allocation_count() == 0);
}

} // namespace

TEST_CASE("beat-repeat re-prepare failure preserves the active configuration",
          "[signal][beat-repeat][prepare]") {
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 64, 8, {1000, 1}}));
    REQUIRE_FALSE(kernel.prepare({1, 16'777'217, 8, {1000, 1}}));
    REQUIRE(kernel.prepared());
    REQUIRE(kernel.channels() == 1);
    REQUIRE(kernel.history_capacity_frames() == 64);
    REQUIRE(kernel.max_tail_samples() == 8);
}

TEST_CASE("beat-repeat moves leave the source safely unprepared",
          "[signal][beat-repeat][prepare][move]") {
    BeatRepeatKernel source;
    REQUIRE(source.prepare({1, 64, 8, {1000, 1}}));
    BeatRepeatKernel moved(std::move(source));
    REQUIRE(moved.prepared());
    REQUIRE_FALSE(source.prepared());
    REQUIRE(source.channels() == 0);

    BeatRepeatKernel assigned;
    REQUIRE(assigned.prepare({1, 32, 4, {1000, 1}}));
    assigned = std::move(moved);
    REQUIRE(assigned.prepared());
    REQUIRE(assigned.history_capacity_frames() == 64);
    REQUIRE_FALSE(moved.prepared());
    REQUIRE(moved.channels() == 0);
}

TEST_CASE("beat-repeat trigger requires preparation and a matching tempo-map rate",
          "[signal][beat-repeat][prepare][timebase]") {
    const auto tempo_1k = constant_tempo();
    BeatRepeatKernel kernel;
    const auto unprepared = kernel.trigger(tempo_1k, BeatDivision::Eighth, {0});
    REQUIRE_FALSE(unprepared);
    REQUIRE(unprepared.error == BeatRepeatPlanError::Unprepared);

    REQUIRE(kernel.prepare({1, 2'000, 0, {1000, 1}}));
    const auto tempo_48k = constant_tempo(120.0, 48'000);
    const auto mismatched = kernel.trigger(tempo_48k, BeatDivision::Eighth, {0});
    REQUIRE_FALSE(mismatched);
    REQUIRE(mismatched.error == BeatRepeatPlanError::SampleRateMismatch);
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Idle);

    REQUIRE(kernel.trigger(tempo_1k, BeatDivision::Eighth, {0}));
}

TEST_CASE("beat-repeat processing rejects invalid audio buffers without changing state",
          "[signal][beat-repeat][buffers]") {
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 64, 8, {1000, 1}}));
    kernel.process(nullptr, nullptr, 1, {0});
    REQUIRE(kernel.available_history_frames() == 0);

    const float* missing_input[]{nullptr};
    float sample = 0.0f;
    float* output[]{&sample};
    kernel.process(missing_input, output, 1, {0});
    REQUIRE(kernel.available_history_frames() == 0);

    const float input_sample = 1.0f;
    const float* input[]{&input_sample};
    float* missing_output[]{nullptr};
    kernel.process(input, missing_output, 1, {0});
    REQUIRE(kernel.available_history_frames() == 0);
}

TEST_CASE("one-frame beat-repeat transitions render the destination endpoint",
          "[signal][beat-repeat][transition]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 1, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(1));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));

    auto input = ramp(760);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, static_cast<int>(input.size()), {0});
    REQUIRE_THAT(output[750], WithinAbs(input[500], 1.0e-7f));
    REQUIRE_THAT(output[751], WithinAbs(input[501], 1.0e-7f));
}

TEST_CASE("one-frame beat-repeat gate fades preserve the audible window",
          "[signal][beat-repeat][gate][transition]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 1, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(1));
    REQUIRE(kernel.set_gate(0.5f));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));

    auto input = ramp(900);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, static_cast<int>(input.size()), {0});
    REQUIRE(output[874] != 0.0f);
    REQUIRE(output[875] == 0.0f);
}

TEST_CASE("near-unity beat-repeat gates fade to silence without a hard cutoff",
          "[signal][beat-repeat][gate][transition]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 8, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(1));
    REQUIRE(kernel.set_gate(249.0f / 250.0f));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));

    std::vector<float> input(1'000, 1.0f);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, static_cast<int>(input.size()), {0});
    REQUIRE(output[990] == 1.0f);
    REQUIRE(output[997] < output[990]);
    REQUIRE(output[998] == 0.0f);
    REQUIRE(output[999] == 0.0f);
}

TEST_CASE("beat-repeat sanitizes non-finite dry and captured audio",
          "[signal][beat-repeat][finite]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 0, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(1));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));

    auto input = ramp(1'000);
    input[600] = std::numeric_limits<float>::quiet_NaN();
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, static_cast<int>(input.size()), {0});
    REQUIRE(output[600] == 0.0f);
    REQUIRE(output[850] == 0.0f);
    REQUIRE(std::all_of(output.begin(), output.end(),
                        [](float value) { return std::isfinite(value); }));
}

TEST_CASE("beat-repeat sanitizes finite transition overflow",
          "[signal][beat-repeat][finite][transition]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 3, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(1));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));

    std::vector<float> input(760, std::numeric_limits<float>::max());
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, static_cast<int>(input.size()), {0});
    REQUIRE(std::all_of(output.begin(), output.end(),
                        [](float value) { return std::isfinite(value); }));
}

TEST_CASE("beat-repeat plans use exact compiled tempo-map grid edges",
          "[signal][beat-repeat][timebase]") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 180.0, TempoCurve::Constant},
    };
    const auto tempo = tempo_map(points, 48'000);
    for (const auto division : {BeatDivision::Quarter, BeatDivision::Eighth,
                                BeatDivision::SixteenthTriplet, BeatDivision::EighthDotted}) {
        const auto quantum = division_ticks(division).value().value;
        for (const auto edge_tick : {quantum, 2 * quantum, 7 * quantum}) {
            const auto edge_sample = tempo.ticks_to_samples({edge_tick});
            const auto previous_sample = tempo.ticks_to_samples({edge_tick - quantum});
            const auto plan = lower_beat_repeat_capture(tempo, division, {edge_sample.value - 1});
            REQUIRE(plan);
            REQUIRE(plan.plan.edge_tick == TickPosition{edge_tick});
            REQUIRE(plan.plan.edge_sample == edge_sample);
            REQUIRE(plan.plan.capture_frames == edge_sample.value - previous_sample.value);
        }
    }

    REQUIRE_FALSE(lower_beat_repeat_capture(tempo, static_cast<BeatDivision>(255), {0}));
    const auto maximum = lower_beat_repeat_capture(tempo, BeatDivision::Quarter,
                                                   {std::numeric_limits<std::int64_t>::max()});
    REQUIRE_FALSE(maximum);
    REQUIRE((maximum.error == BeatRepeatPlanError::TickRangeExceeded ||
             maximum.error == BeatRepeatPlanError::SampleRangeExceeded));
    const auto minimum = lower_beat_repeat_capture(tempo, BeatDivision::Sixteenth,
                                                   {std::numeric_limits<std::int64_t>::min()});
    REQUIRE_FALSE(minimum);
    REQUIRE((minimum.error == BeatRepeatPlanError::TickRangeExceeded ||
             minimum.error == BeatRepeatPlanError::SampleRangeExceeded));
    const std::array slow_point{TempoPoint{{0}, 1.0, TempoCurve::Constant}};
    const auto slow_tempo = tempo_map(slow_point, 768'000);
    const auto saturated_minimum = lower_beat_repeat_capture(
        slow_tempo, BeatDivision::Sixteenth, {std::numeric_limits<std::int64_t>::min()});
    REQUIRE_FALSE(saturated_minimum);
    REQUIRE(saturated_minimum.error == BeatRepeatPlanError::SampleRangeExceeded);
    const auto saturated_maximum = lower_beat_repeat_capture(
        slow_tempo, BeatDivision::Sixteenth, {std::numeric_limits<std::int64_t>::max() - 1});
    REQUIRE_FALSE(saturated_maximum);
    REQUIRE(saturated_maximum.error == BeatRepeatPlanError::SampleRangeExceeded);

    const std::array sparse_point{TempoPoint{{0}, 120.0, TempoCurve::Constant}};
    const auto sparse_compile = CompiledTempoMap::compile(sparse_point, {1, 1'000'000'000});
    REQUIRE(sparse_compile);
    const auto sparse =
        lower_beat_repeat_capture(sparse_compile.value(), BeatDivision::Sixteenth, {0});
    REQUIRE(sparse);
    REQUIRE(sparse.plan.edge_sample.value > 0);
}

TEST_CASE("beat-repeat captures the exact preceding interval at the armed edge",
          "[signal][beat-repeat][capture]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 0, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(1));
    const auto plan = kernel.trigger(tempo, BeatDivision::Eighth, {510});
    REQUIRE(plan);
    REQUIRE(plan.plan.edge_sample == SamplePosition{750});
    REQUIRE(plan.plan.capture_frames == 250);

    auto input = ramp(1'050);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, static_cast<int>(input.size()), {0});

    REQUIRE_THAT(output[750], WithinAbs(input[500], 1.0e-7f));
    REQUIRE_THAT(output[999], WithinAbs(input[749], 1.0e-7f));
    REQUIRE(output[749] == input[749]);
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Idle);
}

TEST_CASE("beat-repeat output is invariant to process block partitioning",
          "[signal][beat-repeat][partition]") {
    const auto tempo = constant_tempo();
    const std::array one_block{1'300};
    const std::array irregular{1, 7, 64, 3, 127, 11, 256};
    const auto whole = render_partitioned(tempo, one_block, 1'300);
    const auto split = render_partitioned(tempo, irregular, 1'300);
    REQUIRE(split == whole);
}

TEST_CASE("beat-repeat rejects fresh and over-capacity capture without clamping",
          "[signal][beat-repeat][history]") {
    const auto tempo = constant_tempo();

    SECTION("fresh history") {
        BeatRepeatKernel kernel;
        REQUIRE(kernel.prepare({1, 600, 0, {1000, 1}}));
        REQUIRE(kernel.trigger(tempo, BeatDivision::Quarter, {0}));
        auto input = ramp(100);
        std::vector<float> output(input.size());
        const float* inputs[]{input.data()};
        float* outputs[]{output.data()};
        kernel.process(inputs, outputs, 100, {450});
        REQUIRE(kernel.status() == BeatRepeatKernel::Status::CaptureRejectedInsufficientHistory);
        REQUIRE(kernel.last_capture_rejected());
        REQUIRE(output == input);

        const auto state = kernel.snapshot();
        BeatRepeatKernel restored;
        REQUIRE(restored.prepare({1, 600, 0, {1000, 1}}));
        REQUIRE(restored.restore(state));
        REQUIRE(restored.status() ==
                BeatRepeatKernel::Status::CaptureRejectedInsufficientHistory);
        REQUIRE(restored.last_capture_rejected());
    }

    SECTION("exact capacity succeeds and one larger division rejects") {
        BeatRepeatKernel exact;
        REQUIRE(exact.prepare({1, 250, 0, {1000, 1}}));
        REQUIRE(exact.trigger(tempo, BeatDivision::Eighth, {510}));
        auto input = ramp(1'100);
        std::vector<float> output(input.size());
        const float* inputs[]{input.data()};
        float* outputs[]{output.data()};
        exact.process(inputs, outputs, 1'100, {0});
        REQUIRE_FALSE(exact.last_capture_rejected());

        BeatRepeatKernel over;
        REQUIRE(over.prepare({1, 250, 0, {1000, 1}}));
        const auto rejected = over.trigger(tempo, BeatDivision::Quarter, {510});
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error == BeatRepeatPlanError::HistoryCapacityExceeded);
        std::fill(output.begin(), output.end(), 0.0f);
        over.process(inputs, outputs, 1'100, {0});
        REQUIRE_FALSE(over.last_capture_rejected());
        REQUIRE(over.status() == BeatRepeatKernel::Status::Idle);
    }
}

TEST_CASE("beat-repeat reverse and alternate modes own repeat direction",
          "[signal][beat-repeat][reverse]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 0, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(2));
    REQUIRE(kernel.set_reverse(BeatRepeatKernel::ReverseMode::Alternate));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(1'300);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, 1'300, {0});
    for (int frame = 0; frame < 250; ++frame) {
        REQUIRE_THAT(output[750 + frame], WithinAbs(input[500 + frame], 1.0e-7f));
        REQUIRE_THAT(output[1'000 + frame], WithinAbs(input[749 - frame], 1.0e-7f));
    }
}

TEST_CASE("lowering the live repeat limit releases to a restorable snapshot",
          "[signal][beat-repeat][state][controls]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 8, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(0));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(1'300);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, static_cast<int>(input.size()), {0});
    REQUIRE(kernel.completed_repeats() >= 1);
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {1'300}));
    REQUIRE_FALSE(kernel.can_snapshot());

    REQUIRE(kernel.set_repeat_count(1));
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Releasing);
    const auto state = kernel.snapshot();
    REQUIRE(state.resumable);
    REQUIRE_FALSE(state.active);

    BeatRepeatKernel restored;
    REQUIRE(restored.prepare({1, 2'000, 8, {1000, 1}}));
    REQUIRE(restored.restore(state));
    REQUIRE(restored.status() == BeatRepeatKernel::Status::Idle);
}

TEST_CASE("finite repeat completion cancels a queued retrigger at the same edge",
          "[signal][beat-repeat][retrigger][controls]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 8, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(0));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(1'510);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, 1'300, {0});
    REQUIRE(kernel.completed_repeats() >= 1);

    REQUIRE(kernel.set_repeat_count(kernel.completed_repeats() + 1));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {1'300}));
    const float* tail_inputs[]{input.data() + 1'300};
    float* tail_outputs[]{output.data() + 1'300};
    kernel.process(tail_inputs, tail_outputs, 210, {1'300});
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Idle);
    REQUIRE(kernel.can_snapshot());
    REQUIRE_FALSE(kernel.snapshot().active);
}

TEST_CASE("beat-repeat reverse seek uses captured-frame coordinates",
          "[signal][beat-repeat][reverse][seek]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 0, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(0));
    REQUIRE(kernel.set_reverse(BeatRepeatKernel::ReverseMode::On));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(901);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, 900, {0});
    REQUIRE(kernel.seek(0));
    const float* tail_inputs[]{input.data() + 900};
    float* tail_outputs[]{output.data() + 900};
    kernel.process(tail_inputs, tail_outputs, 1, {900});
    REQUIRE_THAT(output[900], WithinAbs(input[500], 1.0e-7f));
}

TEST_CASE("beat-repeat retrigger captures dry history rather than its wet output",
          "[signal][beat-repeat][retrigger]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 0, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(0));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    std::vector<float> first_input(900, 0.0f);
    for (int frame = 500; frame < 750; ++frame)
        first_input[static_cast<std::size_t>(frame)] = 1.0f;
    std::vector<float> first_output(first_input.size());
    const float* first_inputs[]{first_input.data()};
    float* first_outputs[]{first_output.data()};
    kernel.process(first_inputs, first_outputs, 900, {0});
    REQUIRE(first_output[800] == 1.0f);

    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {900}));
    std::array<float, 350> dry_zeros{};
    std::array<float, 350> retriggered{};
    const float* second_inputs[]{dry_zeros.data()};
    float* second_outputs[]{retriggered.data()};
    kernel.process(second_inputs, second_outputs, 350, {900});
    REQUIRE(retriggered[99] == 1.0f);
    for (int frame = 100; frame < 350; ++frame)
        REQUIRE(retriggered[static_cast<std::size_t>(frame)] == 0.0f);

    BeatRepeatKernel in_place;
    REQUIRE(in_place.prepare({1, 2'000, 0, {1000, 1}}));
    REQUIRE(in_place.set_repeat_count(0));
    REQUIRE(in_place.trigger(tempo, BeatDivision::Eighth, {510}));
    std::vector<float> aliased(1'250, 0.0f);
    for (int frame = 500; frame < 750; ++frame)
        aliased[static_cast<std::size_t>(frame)] = 1.0f;
    const float* aliased_input[]{aliased.data()};
    float* aliased_output[]{aliased.data()};
    in_place.process(aliased_input, aliased_output, 900, {0});
    REQUIRE(in_place.trigger(tempo, BeatDivision::Eighth, {900}));
    const float* aliased_tail_input[]{aliased.data() + 900};
    float* aliased_tail_output[]{aliased.data() + 900};
    in_place.process(aliased_tail_input, aliased_tail_output, 350, {900});
    REQUIRE(aliased[999] == 1.0f);
    for (int frame = 1'000; frame < 1'250; ++frame)
        REQUIRE(aliased[static_cast<std::size_t>(frame)] == 0.0f);
}

TEST_CASE("beat-repeat replacement transition starts from the current output",
          "[signal][beat-repeat][retrigger][transition]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 32, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(0));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    std::vector<float> first_input(800, 0.0f);
    for (int frame = 500; frame < 750; ++frame)
        first_input[static_cast<std::size_t>(frame)] = 1.0f;
    std::vector<float> first_output(first_input.size());
    const float* first_inputs[]{first_input.data()};
    float* first_outputs[]{first_output.data()};
    kernel.process(first_inputs, first_outputs, 800, {0});

    REQUIRE(kernel.trigger(tempo, BeatDivision::Sixteenth, {800}));
    std::array<float, 200> dry{};
    std::array<float, 200> output{};
    const float* inputs[]{dry.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, 200, {800});
    REQUIRE(output[74] == 1.0f);
    REQUIRE(output[75] == 1.0f);
}

TEST_CASE("beat-repeat gate, stop, seek, and transport reset are deterministic",
          "[signal][beat-repeat][lifecycle]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 8, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(0));
    REQUIRE(kernel.set_gate(0.5f));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(1'100);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, 900, {0});
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Active);
    REQUIRE(output[880] == 0.0f);
    REQUIRE(kernel.seek(249));
    kernel.process(inputs, outputs, 32, {900});
    kernel.stop();
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Releasing);
    kernel.process(inputs, outputs, 16, {932});
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Idle);
    REQUIRE(kernel.loop_length() == 0);
    kernel.transport_discontinuity();
    REQUIRE(kernel.available_history_frames() == 0);
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Idle);
}

TEST_CASE("beat-repeat transition removes the planted capture discontinuity",
          "[signal][beat-repeat][transition]") {
    const auto render = [](int transition_frames) {
        const auto tempo = constant_tempo();
        BeatRepeatKernel kernel;
        REQUIRE(kernel.prepare({1, 2'000, transition_frames, {1000, 1}}));
        REQUIRE(kernel.set_repeat_count(1));
        REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
        std::vector<float> input(1'050, -1.0f);
        std::fill(input.begin() + 500, input.begin() + 620, 1.0f);
        std::vector<float> output(input.size());
        const float* inputs[]{input.data()};
        float* outputs[]{output.data()};
        kernel.process(inputs, outputs, static_cast<int>(input.size()), {0});
        return output;
    };
    const auto hard = render(0);
    const auto faded = render(32);
    REQUIRE(maximum_step(hard, 745, 790) > 1.5f);
    REQUIRE(maximum_step(faded, 745, 790) < maximum_step(hard, 745, 790) * 0.25f);
}

TEST_CASE("beat-repeat snapshots active playback and rejects nonfinite controls",
          "[signal][beat-repeat][state]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel source;
    REQUIRE(source.prepare({1, 2'000, 0, {1000, 1}}));
    REQUIRE(source.set_repeat_count(0));
    REQUIRE_FALSE(source.set_gate(std::numeric_limits<float>::quiet_NaN()));
    REQUIRE(source.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(1'100);
    std::vector<float> source_output(input.size());
    const float* inputs[]{input.data()};
    float* source_outputs[]{source_output.data()};
    source.process(inputs, source_outputs, 900, {0});
    const auto state = source.snapshot();
    auto invalid_state = state;
    invalid_state.sampler[1] = std::numeric_limits<float>::infinity();

    BeatRepeatKernel restored;
    REQUIRE(restored.prepare({1, 2'000, 0, {1000, 1}}));
    BeatRepeatKernel wrong_rate;
    REQUIRE(wrong_rate.prepare({1, 2'000, 0, {48'000, 1}}));
    REQUIRE_FALSE(wrong_rate.restore(state));
    REQUIRE_FALSE(restored.restore(wrong_rate.snapshot()));
    std::array<float, 64> pre_restore_dry{};
    const float* pre_restore_inputs[]{pre_restore_dry.data()};
    float* pre_restore_outputs[]{pre_restore_dry.data()};
    restored.process(pre_restore_inputs, pre_restore_outputs, 64, {0});
    REQUIRE(restored.available_history_frames() == 64);
    REQUIRE(restored.restore(state));
    REQUIRE(restored.available_history_frames() == 0);
    const auto require_rejected_without_mutation =
        [&](const BeatRepeatKernel::Snapshot& malformed) {
            const auto before = restored.snapshot();
            REQUIRE_FALSE(restored.restore(malformed));
            const auto after = restored.snapshot();
            REQUIRE(after.sampler == before.sampler);
            REQUIRE(after.loop_position == before.loop_position);
            REQUIRE(restored.status() == BeatRepeatKernel::Status::Active);
        };
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.last_output.front() = std::numeric_limits<float>::infinity();
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.sampler.pop_back();
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.sampler.push_back(0.0f);
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.sampler[3] = 0.5f;
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.sampler[3] = std::numeric_limits<float>::max();
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.transition = BeatRepeatKernel::TransitionKind::HeldToDry;
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.sample_rate = {48'000, 1};
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.repeat_count = 1;
    invalid_state.completed_repeats = 1;
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.active = false;
    require_rejected_without_mutation(invalid_state);
    invalid_state = state;
    invalid_state.completed_repeats = 1;
    invalid_state.alternate_repeat = false;
    require_rejected_without_mutation(invalid_state);

    BeatRepeatKernel saturated;
    REQUIRE(saturated.prepare({1, 2'000, 0, {1000, 1}}));
    auto saturated_state = state;
    saturated_state.completed_repeats = std::numeric_limits<int>::max();
    saturated_state.alternate_repeat = false;
    REQUIRE(saturated.restore(saturated_state));
    std::array<float, 100> saturated_dry{};
    std::array<float, 100> saturated_output{};
    const float* saturated_inputs[]{saturated_dry.data()};
    float* saturated_outputs[]{saturated_output.data()};
    saturated.process(saturated_inputs, saturated_outputs, 100, {0});
    REQUIRE(saturated.completed_repeats() == std::numeric_limits<int>::max());
    REQUIRE(saturated.snapshot().alternate_repeat);
    std::array<float, 128> source_tail{};
    std::array<float, 128> restored_tail{};
    std::array<float, 128> dry{};
    const float* tail_inputs[]{dry.data()};
    float* source_tail_outputs[]{source_tail.data()};
    float* restored_tail_outputs[]{restored_tail.data()};
    source.process(tail_inputs, source_tail_outputs, 128, {900});
    restored.process(tail_inputs, restored_tail_outputs, 128, {900});
    REQUIRE(restored_tail == source_tail);
    REQUIRE(restored.latency_samples() == 0);
    REQUIRE(restored.retained_bytes() > 0);
}

TEST_CASE("beat-repeat snapshots preserve an active transition",
          "[signal][beat-repeat][state][transition]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel source;
    REQUIRE(source.prepare({1, 2'000, 32, {1000, 1}}));
    REQUIRE(source.set_repeat_count(0));
    REQUIRE(source.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(760);
    std::vector<float> initial_output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{initial_output.data()};
    source.process(inputs, outputs, 760, {0});
    const auto state = source.snapshot();
    REQUIRE(state.transition == BeatRepeatKernel::TransitionKind::DryToWet);
    REQUIRE(state.transition_position == 10);
    REQUIRE_FALSE(source.set_transition_frames(8));

    BeatRepeatKernel restored;
    REQUIRE(restored.prepare({1, 2'000, 32, {1000, 1}}));
    REQUIRE(restored.restore(state));
    BeatRepeatKernel incompatible;
    REQUIRE(incompatible.prepare({1, 2'000, 16, {1000, 1}}));
    REQUIRE(incompatible.restore(state));
    std::array<float, 64> dry{};
    std::array<float, 64> source_output{};
    std::array<float, 64> restored_output{};
    const float* continuation_inputs[]{dry.data()};
    float* source_outputs[]{source_output.data()};
    float* restored_outputs[]{restored_output.data()};
    source.process(continuation_inputs, source_outputs, 64, {760});
    restored.process(continuation_inputs, restored_outputs, 64, {760});
    REQUIRE(restored_output == source_output);
    const auto steady_state = source.snapshot();
    REQUIRE(steady_state.transition == BeatRepeatKernel::TransitionKind::None);
    BeatRepeatKernel steady_restored;
    REQUIRE(steady_restored.prepare({1, 2'000, 16, {1000, 1}}));
    REQUIRE(steady_restored.restore(steady_state));
    REQUIRE(steady_restored.snapshot().transition_frames == 32);
    REQUIRE(source.set_transition_frames(8));
}

TEST_CASE("beat-repeat rejects a stale trigger without resetting active playback",
          "[signal][beat-repeat][trigger][stale]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 0, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(0));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(900);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, 900, {0});
    const auto history_before = kernel.available_history_frames();
    const auto stale = kernel.trigger(tempo, BeatDivision::Eighth, {0});
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error == BeatRepeatPlanError::StaleRequest);
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Active);
    REQUIRE(kernel.available_history_frames() == history_before);

    std::array<float, 16> dry{};
    std::array<float, 16> continued{};
    const float* continuation_inputs[]{dry.data()};
    float* continuation_outputs[]{continued.data()};
    kernel.process(continuation_inputs, continuation_outputs, 16, {900});
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Active);
}

TEST_CASE("beat-repeat rejects snapshots while capture history is armed",
          "[signal][beat-repeat][state][armed]") {
    const auto tempo = constant_tempo();

    SECTION("armed from idle") {
        BeatRepeatKernel source;
        REQUIRE(source.prepare({1, 2'000, 0, {1000, 1}}));
        REQUIRE(source.trigger(tempo, BeatDivision::Eighth, {510}));
        REQUIRE_FALSE(source.can_snapshot());
        const auto state = source.snapshot();
        REQUIRE_FALSE(state.resumable);
        BeatRepeatKernel restored;
        REQUIRE(restored.prepare({1, 2'000, 0, {1000, 1}}));
        REQUIRE_FALSE(restored.restore(state));
    }

    SECTION("armed retrigger") {
        BeatRepeatKernel source;
        REQUIRE(source.prepare({1, 2'000, 0, {1000, 1}}));
        REQUIRE(source.set_repeat_count(0));
        REQUIRE(source.trigger(tempo, BeatDivision::Eighth, {510}));
        auto input = ramp(900);
        std::vector<float> output(input.size());
        const float* inputs[]{input.data()};
        float* outputs[]{output.data()};
        source.process(inputs, outputs, 900, {0});
        REQUIRE(source.trigger(tempo, BeatDivision::Eighth, {900}));
        REQUIRE_FALSE(source.snapshot().resumable);
    }
}

TEST_CASE("beat-repeat snapshots a releasing gesture as inactive",
          "[signal][beat-repeat][state][release]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel source;
    REQUIRE(source.prepare({1, 2'000, 32, {1000, 1}}));
    REQUIRE(source.set_repeat_count(0));
    REQUIRE(source.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(900);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    source.process(inputs, outputs, 900, {0});
    source.stop();
    const auto state = source.snapshot();
    REQUIRE_FALSE(state.active);
    REQUIRE(state.sampler.empty());
    REQUIRE(std::all_of(state.last_output.begin(), state.last_output.end(),
                        [](float value) { return value == 0.0f; }));

    BeatRepeatKernel restored;
    REQUIRE(restored.prepare({1, 2'000, 32, {1000, 1}}));
    REQUIRE(restored.restore(state));
    REQUIRE(restored.status() == BeatRepeatKernel::Status::Idle);
    std::array<float, 64> dry{};
    std::array<float, 64> restored_output{};
    std::fill(dry.begin(), dry.end(), 0.25f);
    const float* restored_inputs[]{dry.data()};
    float* restored_outputs[]{restored_output.data()};
    restored.process(restored_inputs, restored_outputs, 64, {900});
    REQUIRE(restored_output == dry);
}

TEST_CASE("beat-repeat does not restart a release at the finite-repeat boundary",
          "[signal][beat-repeat][release][tail]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 32, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(1));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(990);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, 990, {0});
    REQUIRE(kernel.loop_position() == 240);
    kernel.stop();
    REQUIRE(kernel.max_tail_samples() == 32);

    std::array<float, 32> dry{};
    std::array<float, 32> release{};
    for (int frame = 0; frame < 32; ++frame) {
        kernel.stop();
        const float* release_inputs[]{dry.data() + frame};
        float* release_outputs[]{release.data() + frame};
        kernel.process(release_inputs, release_outputs, 1, {990 + frame});
    }
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Idle);
}

TEST_CASE("beat-repeat release absorbs reverse and seek controls",
          "[signal][beat-repeat][release][controls]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 32, {1000, 1}}));
    REQUIRE(kernel.set_repeat_count(0));
    REQUIRE(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
    auto input = ramp(900);
    std::vector<float> output(input.size());
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    kernel.process(inputs, outputs, 900, {0});
    kernel.stop();
    const auto trigger_during_release = kernel.trigger(tempo, BeatDivision::Eighth, {900});
    REQUIRE_FALSE(trigger_during_release);
    REQUIRE(trigger_during_release.error == BeatRepeatPlanError::ReleaseInProgress);
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Releasing);
    REQUIRE(kernel.set_reverse(BeatRepeatKernel::ReverseMode::On));
    REQUIRE_FALSE(kernel.seek(0));

    std::array<float, 32> dry{};
    std::array<float, 32> release{};
    const float* release_inputs[]{dry.data()};
    float* release_outputs[]{release.data()};
    kernel.process(release_inputs, release_outputs, 32, {900});
    REQUIRE(kernel.status() == BeatRepeatKernel::Status::Idle);
}

TEST_CASE("prepared beat-repeat gesture operations allocate no memory",
          "[signal][beat-repeat][rt-safety]") {
    const auto tempo = constant_tempo();
    BeatRepeatKernel kernel;
    REQUIRE(kernel.prepare({1, 2'000, 8, {1000, 1}}));
    std::array<float, 1'100> input{};
    std::array<float, 1'100> output{};
    const float* inputs[]{input.data()};
    float* outputs[]{output.data()};
    bool controls_valid = true;
    bool trigger_valid = true;
    bool seek_valid = true;

    require_allocates_no_memory([&] {
        controls_valid = kernel.set_repeat_count(0) && kernel.set_gate(0.75f) &&
                         kernel.set_reverse(BeatRepeatKernel::ReverseMode::Alternate);
        trigger_valid = static_cast<bool>(kernel.trigger(tempo, BeatDivision::Eighth, {510}));
        kernel.process(inputs, outputs, 900, {0});
        seek_valid = kernel.seek(31);
        kernel.stop();
        kernel.process(inputs, outputs, 16, {900});
        kernel.reset();
    });
    REQUIRE(controls_valid);
    REQUIRE(trigger_valid);
    REQUIRE(seek_valid);
}

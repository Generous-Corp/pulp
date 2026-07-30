#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/finite_stretch_builder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace pulp::signal;

namespace {

constexpr double kSampleRate = 48000.0;

RealtimePitchTimeConfig stretch_config(int max_block) {
    RealtimePitchTimeConfig config;
    config.mode = PitchTimeMode::time_stretch;
    config.quality = PitchTimeQuality::low_latency;
    config.channels = 1;
    config.max_block = max_block;
    config.max_time_ratio = 2.0f;
    return config;
}

std::vector<float> signal(std::size_t frames) {
    std::vector<float> result(frames);
    for (std::size_t i = 0; i < frames; ++i)
        result[i] = 0.4f * std::sin(static_cast<float>(i) * 0.071f)
                  + 0.1f * std::cos(static_cast<float>(i) * 0.193f);
    return result;
}

std::vector<double> signal64(std::size_t frames) {
    const auto source = signal(frames);
    return {source.begin(), source.end()};
}

float ramp_ratio(void* context, std::uint64_t input_frame) noexcept {
    const auto total = *static_cast<const std::uint64_t*>(context);
    return 0.75f + 0.75f * static_cast<float>(input_frame)
                       / static_cast<float>(std::max<std::uint64_t>(total, 1));
}

float invalid_dynamic_ratio(void*, std::uint64_t) noexcept {
    return std::numeric_limits<float>::quiet_NaN();
}

struct CountingRatio {
    int calls = 0;
    std::array<std::uint64_t, 128> positions {};
};

float counting_ratio(void* context, std::uint64_t position) noexcept {
    auto& counter = *static_cast<CountingRatio*>(context);
    if (counter.calls < static_cast<int>(counter.positions.size()))
        counter.positions[static_cast<std::size_t>(counter.calls)] = position;
    ++counter.calls;
    return 2.0f;
}

template <typename SampleType>
std::vector<SampleType> direct_boundary_render_t(const std::vector<SampleType>& input,
                                                 int max_block) {
    auto config = stretch_config(max_block);
    RealtimePitchTimeProcessorT<SampleType> processor;
    REQUIRE(processor.prepare(kSampleRate, config) == PitchTimePrepareStatus::prepared);

    std::vector<SampleType> output;
    std::vector<SampleType> drain(static_cast<std::size_t>(max_block));
    SampleType* drain_ptrs[] = {drain.data()};
    std::uint64_t consumed = 0;
    auto input_frames = static_cast<std::uint64_t>(input.size());
    bool finalizing = false;
    for (int guard = 0; guard < 20000; ++guard) {
        if (processor.available_stretched() > 0) {
            const int take = std::min(processor.available_stretched(), max_block);
            REQUIRE(processor.read_stretched(drain_ptrs, take) == take);
            output.insert(output.end(), drain.begin(), drain.begin() + take);
            continue;
        }
        const int until = processor.samples_until_next_analysis_frame();
        REQUIRE(until > 0);
        if (!finalizing && consumed < input.size()) {
            const int count = static_cast<int>(std::min<std::uint64_t>(
                input.size() - consumed,
                static_cast<std::uint64_t>(std::min(max_block, until))));
            if (count == until)
                processor.set_time_ratio(ramp_ratio(&input_frames, consumed + count));
            const SampleType* source[] = {input.data() + consumed};
            const auto status = processor.feed(source, count);
            if (status == PitchTimeStreamFeedStatus::backpressure) continue;
            REQUIRE(status == PitchTimeStreamFeedStatus::accepted);
            consumed += static_cast<std::uint64_t>(count);
            continue;
        }
        finalizing = true;
        const int run = std::min(max_block, until);
        if (run == until)
            processor.set_time_ratio(ramp_ratio(&input_frames, input_frames));
        const auto status = processor.finalize(run);
        if (status == PitchTimeStreamFinalizeStatus::complete) return output;
        REQUIRE((status == PitchTimeStreamFinalizeStatus::draining
                 || status == PitchTimeStreamFinalizeStatus::backpressure));
    }
    FAIL("finite direct render did not terminate");
    return {};
}

std::vector<float> direct_boundary_render(const std::vector<float>& input, int max_block) {
    return direct_boundary_render_t(input, max_block);
}

template <typename SampleType>
std::vector<SampleType> naive_block_render_t(const std::vector<SampleType>& input,
                                             int max_block) {
    auto config = stretch_config(max_block);
    RealtimePitchTimeProcessorT<SampleType> processor;
    REQUIRE(processor.prepare(kSampleRate, config) == PitchTimePrepareStatus::prepared);
    std::vector<SampleType> output;
    std::vector<SampleType> drain(static_cast<std::size_t>(max_block));
    SampleType* drain_ptrs[] = {drain.data()};
    std::uint64_t input_frames = input.size();
    std::uint64_t consumed = 0;
    for (int guard = 0; guard < 20000; ++guard) {
        if (processor.available_stretched() > 0) {
            const int take = std::min(processor.available_stretched(), max_block);
            REQUIRE(processor.read_stretched(drain_ptrs, take) == take);
            output.insert(output.end(), drain.begin(), drain.begin() + take);
            continue;
        }
        if (consumed < input_frames) {
            const int count = static_cast<int>(std::min<std::uint64_t>(
                input_frames - consumed, static_cast<std::uint64_t>(max_block)));
            // Deliberately wrong negative control: one ratio for an arbitrary
            // work block, rather than the exact analysis boundary it crosses.
            processor.set_time_ratio(ramp_ratio(&input_frames, consumed + count));
            const SampleType* source[] = {input.data() + consumed};
            const auto status = processor.feed(source, count);
            if (status == PitchTimeStreamFeedStatus::backpressure) continue;
            REQUIRE(status == PitchTimeStreamFeedStatus::accepted);
            consumed += static_cast<std::uint64_t>(count);
            continue;
        }
        processor.set_time_ratio(ramp_ratio(&input_frames, input_frames));
        const auto status = processor.finalize();
        if (status == PitchTimeStreamFinalizeStatus::complete) return output;
        REQUIRE((status == PitchTimeStreamFinalizeStatus::draining
                 || status == PitchTimeStreamFinalizeStatus::backpressure));
    }
    FAIL("naive finite render did not terminate");
    return {};
}

template <typename SampleType>
struct BuilderRenderT {
    FiniteStretchStepStatus status = FiniteStretchStepStatus::progress;
    FiniteStretchFailure failure = FiniteStretchFailure::none;
    std::uint64_t written = 0;
    std::vector<SampleType> output;
};

using BuilderRender = BuilderRenderT<float>;

template <typename SampleType>
BuilderRenderT<SampleType> builder_render_t(const std::vector<SampleType>& input,
                                           std::uint64_t target, int max_block) {
    BuilderRenderT<SampleType> result;
    result.output.resize(static_cast<std::size_t>(target));
    const SampleType* source[] = {input.data()};
    SampleType* destination[] = {result.output.data()};
    auto processor = stretch_config(max_block);
    FiniteStretchConfigT<SampleType> config;
    config.sample_rate = kSampleRate;
    config.processor = processor;
    config.input = source;
    config.input_frames = input.size();
    config.output = destination;
    config.output_capacity_frames = target;
    config.target_frames = target;
    config.ratio_at_input_frame = ramp_ratio;
    config.ratio_context = const_cast<std::uint64_t*>(&config.input_frames);

    FiniteStretchBuilderT<SampleType> builder;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::prepared);
    for (int guard = 0; guard < 50000; ++guard) {
        result.status = builder.step();
        if (result.status != FiniteStretchStepStatus::progress) break;
        REQUIRE(builder.last_work_unit() != FiniteStretchWorkUnit::none);
    }
    result.failure = builder.failure();
    result.written = builder.output_frames_written();
    return result;
}

BuilderRender builder_render(const std::vector<float>& input, std::uint64_t target,
                             int max_block) {
    return builder_render_t(input, target, max_block);
}

} // namespace

TEST_CASE("Realtime pitch-time exposes exact analysis boundaries and bounded EOF work",
          "[signal][pitch-time][finite-stretch]") {
    RealtimePitchTimeProcessor processor;
    REQUIRE(processor.samples_until_next_analysis_frame() == 0);
    auto config = stretch_config(37);
    REQUIRE(processor.prepare(kSampleRate, config) == PitchTimePrepareStatus::prepared);
    REQUIRE(processor.samples_until_next_analysis_frame() == 1024);

    std::vector<float> input(1000, 0.25f);
    const float* source[] = {input.data()};
    REQUIRE(processor.feed(source, 37) == PitchTimeStreamFeedStatus::accepted);
    REQUIRE(processor.samples_until_next_analysis_frame() == 987);
    REQUIRE(processor.plan_finalize(0).status
            == PitchTimeStreamFinalizePlanStatus::invalid_request);
    REQUIRE(processor.plan_finalize(38).status
            == PitchTimeStreamFinalizePlanStatus::invalid_request);
    REQUIRE(processor.plan_finalize(37).status == PitchTimeStreamFinalizePlanStatus::ready);
    REQUIRE(processor.samples_until_next_analysis_frame() == 987);
    REQUIRE(processor.finalize(0) == PitchTimeStreamFinalizeStatus::invalid_request);
    REQUIRE(processor.finalize(38) == PitchTimeStreamFinalizeStatus::invalid_request);
    REQUIRE(processor.finalize(37) != PitchTimeStreamFinalizeStatus::invalid_request);
    REQUIRE(processor.samples_until_next_analysis_frame() == 950);
}

TEST_CASE("Finite stretch builder is block-schedule independent for ratio ramps",
          "[signal][pitch-time][finite-stretch]") {
    // Use the portable scalar FFT path so this assertion measures block
    // scheduling itself rather than cross-instance vDSP rounding choices.
    const auto input = signal64(1537);
    const auto reference = direct_boundary_render_t(input, 113);
    const auto repeated_reference = direct_boundary_render_t(input, 113);
    const auto negative_control = naive_block_render_t(input, 113);
    REQUIRE_FALSE(reference.empty());
    REQUIRE(repeated_reference == reference);
    REQUIRE(negative_control != reference);

    const auto tiny = builder_render_t(input, reference.size(), 37);
    const auto wide = builder_render_t(input, reference.size(), 251);
    REQUIRE(tiny.status == FiniteStretchStepStatus::complete);
    REQUIRE(wide.status == FiniteStretchStepStatus::complete);
    REQUIRE(tiny.failure == FiniteStretchFailure::none);
    REQUIRE(wide.failure == FiniteStretchFailure::none);
    REQUIRE(tiny.written == reference.size());
    REQUIRE(wide.written == reference.size());
    const auto tiny_mismatch = std::mismatch(tiny.output.begin(), tiny.output.end(),
                                             reference.begin());
    double tiny_max_difference = 0.0;
    for (std::size_t i = 0; i < reference.size(); ++i)
        tiny_max_difference =
            std::max(tiny_max_difference, std::abs(tiny.output[i] - reference[i]));
    INFO("tiny first mismatch = "
         << std::distance(tiny.output.begin(), tiny_mismatch.first)
         << ", max difference = " << tiny_max_difference);
    REQUIRE(tiny.output == reference);
    REQUIRE(wide.output == reference);
}

TEST_CASE("Finite stretch builder reports exact-length and ratio failures",
          "[signal][pitch-time][finite-stretch]") {
    const auto input = signal(1025);
    const auto reference = direct_boundary_render(input, 97);
    REQUIRE(reference.size() > 1);

    const auto short_target = builder_render(input, reference.size() - 1, 97);
    REQUIRE(short_target.status == FiniteStretchStepStatus::failed);
    REQUIRE(short_target.failure == FiniteStretchFailure::output_too_long);

    const auto long_target = builder_render(input, reference.size() + 1, 97);
    REQUIRE(long_target.status == FiniteStretchStepStatus::failed);
    REQUIRE(long_target.failure == FiniteStretchFailure::output_too_short);
    REQUIRE(long_target.written == reference.size());

    std::vector<float> output(reference.size());
    const float* source[] = {input.data()};
    float* destination[] = {output.data()};
    auto processor = stretch_config(97);
    FiniteStretchConfig config;
    config.processor = processor;
    config.input = source;
    config.input_frames = input.size();
    config.output = destination;
    config.output_capacity_frames = output.size();
    config.target_frames = output.size();
    config.constant_time_ratio = std::numeric_limits<float>::quiet_NaN();
    FiniteStretchBuilder invalid_ratio;
    REQUIRE(invalid_ratio.prepare(config) == FiniteStretchPrepareStatus::invalid_ratio);
    config.constant_time_ratio = 1.0f;
    config.ratio_at_input_frame = invalid_dynamic_ratio;
    REQUIRE(invalid_ratio.prepare(config) == FiniteStretchPrepareStatus::prepared);
    FiniteStretchStepStatus invalid_status = FiniteStretchStepStatus::progress;
    for (int guard = 0; guard < 100 && invalid_status == FiniteStretchStepStatus::progress;
         ++guard)
        invalid_status = invalid_ratio.step();
    REQUIRE(invalid_status == FiniteStretchStepStatus::failed);
    REQUIRE(invalid_ratio.failure() == FiniteStretchFailure::invalid_ratio);
}

TEST_CASE("Finite stretch builder validates target capacities before processor allocation",
          "[signal][pitch-time][finite-stretch]") {
    float input = 0.0f;
    float output = 0.0f;
    const float* source[] = {&input};
    float* destination[] = {&output};
    FiniteStretchConfig config;
    config.processor = stretch_config(64);
    config.input = source;
    config.input_frames = 1;
    config.output = destination;
    config.output_capacity_frames = 2;
    config.target_frames = 3;

    FiniteStretchBuilder builder;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::invalid_target);

    config.target_frames = 2;
    config.target_max_bytes = sizeof(float);
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::unrepresentable_capacity);
    REQUIRE(builder.processor_prepare_status() == PitchTimePrepareStatus::invalid_sample_rate);

    config.input_frames = 257;
    config.output_capacity_frames = 0;
    config.target_frames = 0;
    config.target_max_bytes = 1024;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::unrepresentable_capacity);

    config.input_frames = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())
                        / sizeof(float) + 1u;
    config.target_max_bytes = std::numeric_limits<std::uint32_t>::max();
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::unrepresentable_capacity);

    FiniteStretchConfig64 double_config;
    const double double_input = 0.0;
    const double* double_source[] = {&double_input};
    double_config.processor = stretch_config(64);
    double_config.input = double_source;
    double_config.input_frames = 129;
    double_config.target_max_bytes = 1024;
    FiniteStretchBuilder64 double_builder;
    REQUIRE(double_builder.prepare(double_config)
            == FiniteStretchPrepareStatus::unrepresentable_capacity);
}

TEST_CASE("Finite stretch capacity is per planar allocation and prepare reuse clears state",
          "[signal][pitch-time][finite-stretch]") {
    std::vector<float> input(64, 0.25f);
    std::vector<float> left(64);
    std::vector<float> right(64);
    const float* sources[] = {input.data(), input.data()};
    float* destinations[] = {left.data(), right.data()};
    FiniteStretchConfig config;
    config.processor = stretch_config(64);
    config.processor.channels = 2;
    config.input = sources;
    config.input_frames = input.size();
    config.output = destinations;
    config.output_capacity_frames = left.size();
    config.target_frames = left.size();
    // Each plane fits exactly; their aggregate intentionally does not.
    config.target_max_bytes = left.size() * sizeof(float);

    FiniteStretchBuilder builder;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::processor_prepare_failed);

    config.target_max_bytes = kTargetAddressMaximumBytes;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::prepared);
    REQUIRE(builder.processor_prepare_status() == PitchTimePrepareStatus::prepared);
    config.target_frames = config.output_capacity_frames + 1;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::invalid_target);
    REQUIRE(builder.processor_prepare_status() == PitchTimePrepareStatus::invalid_sample_rate);
    REQUIRE(builder.input_frames_consumed() == 0);
    REQUIRE(builder.output_frames_written() == 0);

    config.target_frames = config.output_capacity_frames;
    config.output = destinations;
    destinations[1] = destinations[0];
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::invalid_output_layout);
    destinations[1] = right.data();
    destinations[0] = const_cast<float*>(sources[0]);
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::invalid_output_layout);
    destinations[0] = reinterpret_cast<float*>(
        std::numeric_limits<std::uintptr_t>::max() - sizeof(float) + 1u);
    destinations[1] = right.data();
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::invalid_output_layout);
}

TEST_CASE("Finite stretch counter admission includes EOF and signed output headroom",
          "[signal][pitch-time][finite-stretch]") {
    auto config = stretch_config(256);
    constexpr std::uint64_t padding = 1024u + 2u * 256u;
    constexpr auto signed_max = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    REQUIRE(checked_finite_stretch_counter_geometry(config, signed_max - padding + 1u)
            == FiniteStretchCounterGeometryStatus::input_counter_overflow);
    REQUIRE(checked_finite_stretch_counter_geometry(config, signed_max - padding)
            != FiniteStretchCounterGeometryStatus::input_counter_overflow);
    REQUIRE(checked_finite_stretch_counter_geometry(config, (std::uint64_t{1} << 53u))
            == FiniteStretchCounterGeometryStatus::representable);
}

TEST_CASE("Fractional synthesis hops do not lose quarters at large absolute positions",
          "[signal][pitch-time][finite-stretch]") {
    constexpr double large_position = 4503599627370496.0; // 2^52
    constexpr double exact_hop = 256.25;
    double naive_absolute = large_position;
    for (int frame = 0; frame < 4; ++frame) naive_absolute += exact_hop;
    const auto naive_advance = static_cast<std::int64_t>(std::llround(naive_absolute))
                             - static_cast<std::int64_t>(large_position);
    REQUIRE(naive_advance == 1024); // negative control: four quarters vanished

    detail::FractionalSynthesisHopAccumulator accumulator;
    int bounded_advance = 0;
    for (int frame = 0; frame < 4; ++frame)
        bounded_advance += accumulator.advance(exact_hop);
    REQUIRE(bounded_advance == 1025);
    REQUIRE(accumulator.residual() == 0.0);
}

TEST_CASE("Finite stretch builder handles empty and multichannel streams",
          "[signal][pitch-time][finite-stretch]") {
    FiniteStretchConfig empty_config;
    empty_config.processor = stretch_config(31);
    FiniteStretchBuilder empty;
    REQUIRE(empty.prepare(empty_config) == FiniteStretchPrepareStatus::prepared);
    FiniteStretchStepStatus empty_status = FiniteStretchStepStatus::progress;
    for (int guard = 0; guard < 1000 && empty_status == FiniteStretchStepStatus::progress;
         ++guard)
        empty_status = empty.step();
    REQUIRE(empty_status == FiniteStretchStepStatus::complete);
    REQUIRE(empty.output_frames_written() == 0);

    const auto left = signal(1024);
    auto right = left;
    std::reverse(right.begin(), right.end());
    std::vector<float> out_left(1024);
    std::vector<float> out_right(1024);
    const float* sources[] = {left.data(), right.data()};
    float* destinations[] = {out_left.data(), out_right.data()};
    FiniteStretchConfig config;
    config.processor = stretch_config(63);
    config.processor.channels = 2;
    config.input = sources;
    config.input_frames = left.size();
    config.output = destinations;
    config.output_capacity_frames = out_left.size();
    config.target_frames = out_left.size();

    FiniteStretchBuilder builder;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::prepared);
    FiniteStretchStepStatus status = FiniteStretchStepStatus::progress;
    for (int guard = 0; guard < 10000 && status == FiniteStretchStepStatus::progress; ++guard)
        status = builder.step();
    REQUIRE(status == FiniteStretchStepStatus::complete);
    REQUIRE(out_left != out_right);
}

TEST_CASE("Finite stretch builder drains then retries an identical rejected feed",
          "[signal][pitch-time][finite-stretch]") {
    const auto input = signal(16384);
    std::vector<float> output(input.size() * 2);
    const float* source[] = {input.data()};
    float* destination[] = {output.data()};
    FiniteStretchConfig config;
    config.processor = stretch_config(256);
    config.input = source;
    config.input_frames = input.size();
    config.output = destination;
    config.output_capacity_frames = output.size();
    config.target_frames = output.size();
    config.constant_time_ratio = 2.0f;

    FiniteStretchBuilder builder;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::prepared);
    bool observed_backpressure_retry = false;
    for (int guard = 0; guard < 50000; ++guard) {
        const auto before = builder.input_frames_consumed();
        const auto status = builder.step();
        if (builder.last_work_unit() == FiniteStretchWorkUnit::feed
            && builder.input_frames_consumed() == before) {
            REQUIRE(builder.step() == FiniteStretchStepStatus::progress);
            REQUIRE(builder.last_work_unit() == FiniteStretchWorkUnit::drain);
            REQUIRE(builder.input_frames_consumed() == before);
            do {
                REQUIRE(builder.step() == FiniteStretchStepStatus::progress);
            } while (builder.last_work_unit() == FiniteStretchWorkUnit::drain);
            REQUIRE(builder.last_work_unit() == FiniteStretchWorkUnit::feed);
            REQUIRE(builder.input_frames_consumed() > before);
            observed_backpressure_retry = true;
            break;
        }
        REQUIRE(status == FiniteStretchStepStatus::progress);
    }
    REQUIRE(observed_backpressure_retry);
}

TEST_CASE("Finite stretch callback is resolved once for rejected and terminal work",
          "[signal][pitch-time][finite-stretch]") {
    const auto input = signal(16384);
    std::vector<float> output(input.size() * 2);
    const float* source[] = {input.data()};
    float* destination[] = {output.data()};
    CountingRatio counter;
    FiniteStretchConfig config;
    config.processor = stretch_config(256);
    config.input = source;
    config.input_frames = input.size();
    config.output = destination;
    config.output_capacity_frames = output.size();
    config.target_frames = output.size();
    config.ratio_at_input_frame = counting_ratio;
    config.ratio_context = &counter;

    FiniteStretchBuilder builder;
    REQUIRE(builder.prepare(config) == FiniteStretchPrepareStatus::prepared);
    bool awaiting_retry = false;
    int calls_at_rejection = 0;
    bool saw_rejection = false;
    bool saw_finalize_preflight_drain = false;
    FiniteStretchStepStatus status = FiniteStretchStepStatus::progress;
    for (int guard = 0; guard < 50000 && status == FiniteStretchStepStatus::progress; ++guard) {
        const int calls_before = counter.calls;
        status = builder.step();
        if (builder.input_frames_consumed() == input.size()
            && builder.last_work_unit() == FiniteStretchWorkUnit::drain) {
            REQUIRE(counter.calls == calls_before);
            saw_finalize_preflight_drain = true;
        }
        if (awaiting_retry) {
            REQUIRE(counter.calls == calls_at_rejection);
            if (builder.last_work_unit() == FiniteStretchWorkUnit::feed
                && builder.last_work_outcome() == FiniteStretchWorkOutcome::advanced)
                awaiting_retry = false;
        }
        if (builder.last_work_outcome() == FiniteStretchWorkOutcome::backpressure) {
            awaiting_retry = true;
            calls_at_rejection = counter.calls;
            saw_rejection = true;
        }
        if (status == FiniteStretchStepStatus::complete) {
            const int terminal_calls = counter.calls;
            REQUIRE(builder.step() == FiniteStretchStepStatus::complete);
            REQUIRE(builder.last_work_unit() == FiniteStretchWorkUnit::none);
            REQUIRE(counter.calls == terminal_calls);
        }
    }
    REQUIRE(saw_rejection);
    REQUIRE(saw_finalize_preflight_drain);
    REQUIRE(status == FiniteStretchStepStatus::complete);
    REQUIRE(counter.calls < static_cast<int>(counter.positions.size()));
    REQUIRE(counter.positions[0] == 1024);
    for (int i = 1; i < counter.calls; ++i) {
        REQUIRE(counter.positions[static_cast<std::size_t>(i)]
                >= counter.positions[static_cast<std::size_t>(i - 1)]);
        if (counter.positions[static_cast<std::size_t>(i)]
            == counter.positions[static_cast<std::size_t>(i - 1)])
            REQUIRE(counter.positions[static_cast<std::size_t>(i)] == input.size());
    }
    REQUIRE(counter.positions[static_cast<std::size_t>(counter.calls - 1)] == input.size());
}

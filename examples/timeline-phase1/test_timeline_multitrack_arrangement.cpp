#include "timeline_multitrack_arrangement.hpp"
#include "timeline_phase1_example_test_support.hpp"

static_assert(TimelineMultitrackArrangementProcessor::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);

namespace {

constexpr std::size_t kArrangementMeterChangeFrame = 96'000;
constexpr std::size_t kArrangementTempoChangeFrame = 128'000;
constexpr std::size_t kArrangementRenderFrames = kArrangementTempoChangeFrame + 128;
static_assert(kArrangementRenderFrames % 128 == 0);

struct ArrangementRender {
    std::vector<float> left;
    std::vector<float> right;
    playback::TransportSnapshot terminal_transport;
    std::size_t automation_event_count = 0;
    int graph_latency_samples = 0;
    int reported_latency_samples = 0;
    bool meter_change_applied = false;
};

std::vector<std::size_t> fixed_arrangement_partitions() {
    return std::vector<std::size_t>(kArrangementRenderFrames / 128, 128);
}

std::vector<std::size_t> irregular_arrangement_partitions() {
    // The early boundaries isolate the delayed impulse, authored note start/end,
    // PDC-shifted note output/end, and automation endpoint. The long-range
    // boundaries make the meter change controllable at the same sample in both
    // renders and leave an identical final callback beginning at the tempo knot.
    constexpr std::array meaningful_boundaries{
        std::size_t{8},      std::size_t{32},
        std::size_t{40},     std::size_t{48},
        std::size_t{64},     std::size_t{128},
        kArrangementMeterChangeFrame, kArrangementTempoChangeFrame,
        kArrangementRenderFrames,
    };
    constexpr std::array pattern{
        std::size_t{1}, std::size_t{7}, std::size_t{31}, std::size_t{64},
        std::size_t{127},
    };

    std::vector<std::size_t> result;
    std::size_t position = 0;
    std::size_t pattern_index = 0;
    while (position < kArrangementRenderFrames) {
        if (position == kArrangementTempoChangeFrame) {
            const auto frames = kArrangementRenderFrames - position;
            result.push_back(frames);
            position += frames;
            continue;
        }

        auto frames = pattern[pattern_index++ % pattern.size()];
        const auto boundary = std::find_if(
            meaningful_boundaries.begin(), meaningful_boundaries.end(),
            [position](std::size_t candidate) { return candidate > position; });
        frames = std::min(frames, *boundary - position);
        result.push_back(frames);
        position += frames;
    }
    return result;
}

bool partition_ends_at(const std::vector<std::size_t>& partitions, std::size_t boundary) {
    std::size_t position = 0;
    for (const auto frames : partitions) {
        position += frames;
        if (position == boundary)
            return true;
        if (position > boundary)
            return false;
    }
    return false;
}

ArrangementRender render_arrangement(const std::vector<std::size_t>& partitions) {
    TimelineMultitrackArrangementProcessor processor;
    auto context = prepare_context(128);
    context.input_channels = 2;
    processor.prepare(context);
    REQUIRE(processor.engine_prepared());

    ArrangementRender result;
    result.left.reserve(kArrangementRenderFrames);
    result.right.reserve(kArrangementRenderFrames);
    std::size_t position = 0;
    for (const auto frames : partitions) {
        StereoBlock block(frames);
        process_direct(processor, block);
        result.left.insert(result.left.end(), block.left.begin(), block.left.end());
        result.right.insert(result.right.end(), block.right.begin(), block.right.end());
        position += frames;
        if (position == kArrangementMeterChangeFrame) {
            REQUIRE_FALSE(result.meter_change_applied);
            result.meter_change_applied = processor.apply_arrangement_meter_change();
            REQUIRE(result.meter_change_applied);
        }
    }

    REQUIRE(position == kArrangementRenderFrames);
    result.terminal_transport = processor.last_transport();
    // The processor owns the map; retain only the snapshot's value semantics.
    result.terminal_transport.tempo_map = nullptr;
    result.automation_event_count = processor.automation_event_count();
    result.graph_latency_samples = processor.graph_latency_samples();
    result.reported_latency_samples = processor.latency_samples();
    return result;
}

void require_same_stable_transport(const playback::TransportSnapshot& fixed,
                                   const playback::TransportSnapshot& irregular) {
    // tempo_map identities and block_index values belong to separate processor
    // instances/callback partitions. Every other published value is semantic.
    REQUIRE(fixed.sample_rate == irregular.sample_rate);
    REQUIRE(fixed.playback_epoch == irregular.playback_epoch);
    REQUIRE(fixed.frame_count == irregular.frame_count);
    REQUIRE(fixed.meter == irregular.meter);
    REQUIRE(fixed.loop == irregular.loop);
    REQUIRE(fixed.is_playing == irregular.is_playing);
    REQUIRE(fixed.scrubbing == irregular.scrubbing);
    REQUIRE(fixed.transport_changed == irregular.transport_changed);
    REQUIRE(fixed.transport_started == irregular.transport_started);
    REQUIRE(fixed.reset_requested == irregular.reset_requested);
    REQUIRE(fixed.time_sig_changed == irregular.time_sig_changed);
    REQUIRE(fixed.tempo_bpm == irregular.tempo_bpm);
    REQUIRE(fixed.range_count == irregular.range_count);
    REQUIRE(fixed.host_loop_start_beats == irregular.host_loop_start_beats);
    REQUIRE(fixed.host_loop_end_beats == irregular.host_loop_end_beats);
    REQUIRE(fixed.has_precise_host_loop == irregular.has_precise_host_loop);

    for (std::uint8_t index = 0; index < fixed.range_count; ++index) {
        const auto& expected = fixed.ranges[index];
        const auto& actual = irregular.ranges[index];
        CAPTURE(index);
        REQUIRE(expected.sample_offset == actual.sample_offset);
        REQUIRE(expected.frame_count == actual.frame_count);
        REQUIRE(expected.timeline_sample_start == actual.timeline_sample_start);
        REQUIRE(expected.timeline_tick_start == actual.timeline_tick_start);
        REQUIRE(expected.timeline_tick_end == actual.timeline_tick_end);
        REQUIRE(expected.monotonic_start == actual.monotonic_start);
        REQUIRE(expected.monotonic_end == actual.monotonic_end);
        REQUIRE(expected.bar_start == actual.bar_start);
        REQUIRE(expected.tempo_bpm == actual.tempo_bpm);
        REQUIRE(expected.tempo_changed == actual.tempo_changed);
        REQUIRE(expected.discontinuity == actual.discontinuity);
        REQUIRE(expected.host_beat_mapping == actual.host_beat_mapping);
        REQUIRE(expected.host_tick_start == actual.host_tick_start);
        REQUIRE(expected.host_tick_end == actual.host_tick_end);
        REQUIRE(expected.has_precise_host_ticks == actual.has_precise_host_ticks);
        REQUIRE(expected.playback_epoch == actual.playback_epoch);
        REQUIRE(expected.loop_pass_index == actual.loop_pass_index);
    }
}

void require_exact_samples(const std::vector<float>& expected,
                           const std::vector<float>& actual) {
    REQUIRE(expected.size() == actual.size());
    const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual.begin());
    const auto mismatch_index =
        static_cast<std::size_t>(std::distance(expected.begin(), mismatch.first));
    const auto expected_sample =
        mismatch.first == expected.end() ? 0.0f : *mismatch.first;
    const auto actual_sample =
        mismatch.first == expected.end() ? 0.0f : *mismatch.second;
    CAPTURE(mismatch_index, expected_sample, actual_sample);
    REQUIRE(mismatch.first == expected.end());
}

} // namespace

TEST_CASE("timeline multitrack arrangement plays with active PDC") {
    TimelineMultitrackArrangementProcessor processor;
    auto context = prepare_context();
    context.input_channels = 2;
    processor.prepare(context);
    REQUIRE(processor.engine_prepared());
    REQUIRE(processor.graph_latency_samples() ==
            TimelineMultitrackArrangementProcessor::pdc_latency_samples);
    REQUIRE(processor.latency_samples() ==
            TimelineMultitrackArrangementProcessor::pdc_latency_samples);
    REQUIRE(processor.descriptor().input_buses.size() == 1);
    REQUIRE(processor.descriptor().input_buses.front().default_channels == 2);

    StereoBlock block(128);
    process_direct(processor, block);

    constexpr auto latency =
        static_cast<std::size_t>(TimelineMultitrackArrangementProcessor::pdc_latency_samples);
    REQUIRE(std::all_of(block.left.begin(), block.left.begin() + latency,
                        [](float sample) { return sample == 0.0f; }));
    REQUIRE(std::all_of(block.right.begin(), block.right.begin() + latency,
                        [](float sample) { return sample == 0.0f; }));
    REQUIRE(block.left[latency] == 2.0f);
    REQUIRE(block.right[latency] == 2.0f);

    const auto instrument_frame = latency + 32;
    REQUIRE(block.left[32] == 0.0f);
    REQUIRE(block.right[32] == 0.0f);
    REQUIRE(block.left[instrument_frame] > 0.25f);
    REQUIRE(block.left[instrument_frame] < 0.5f);
    REQUIRE(block.right[instrument_frame] == block.left[instrument_frame]);
    REQUIRE(processor.automation_event_count() > 0);
    REQUIRE(processor.last_transport().frame_count == 128);
}

TEST_CASE("timeline multitrack arrangement process is allocation free after prepare") {
    TimelineMultitrackArrangementProcessor processor;
    auto context = prepare_context();
    context.input_channels = 2;
    processor.prepare(context);
    REQUIRE(processor.engine_prepared());
    StereoBlock block(128);
    process_direct(processor, block);
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        process_direct(processor, block);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

TEST_CASE("timeline multitrack arrangement applies its authored meter and tempo changes") {
    TimelineMultitrackArrangementProcessor processor;
    auto context = prepare_context();
    context.input_channels = 2;
    processor.prepare(context);
    REQUIRE(processor.engine_prepared());

    bool meter_change_reached = false;
    for (int block_index = 0; block_index < 1000 && !meter_change_reached; ++block_index) {
        StereoBlock block(128);
        process_direct(processor, block);
        REQUIRE(processor.last_transport().meter == playback::MeterSignature{4, 4});
        meter_change_reached = processor.apply_arrangement_meter_change();
    }
    REQUIRE(meter_change_reached);

    StereoBlock changed_meter(128);
    process_direct(processor, changed_meter);
    REQUIRE(processor.last_transport().meter == playback::MeterSignature{3, 4});
    REQUIRE(processor.last_transport().time_sig_changed);

    bool tempo_change_reached = false;
    for (int block_index = 0; block_index < 1000 && !tempo_change_reached; ++block_index) {
        StereoBlock block(128);
        process_direct(processor, block);
        tempo_change_reached = processor.last_transport().tempo_bpm == 132.0;
    }
    REQUIRE(tempo_change_reached);
}

TEST_CASE("timeline multitrack arrangement is sample exact across block partitions") {
    const auto fixed_partitions = fixed_arrangement_partitions();
    const auto irregular_partitions = irregular_arrangement_partitions();

    REQUIRE(std::accumulate(fixed_partitions.begin(), fixed_partitions.end(), std::size_t{0}) ==
            kArrangementRenderFrames);
    REQUIRE(std::accumulate(irregular_partitions.begin(), irregular_partitions.end(),
                            std::size_t{0}) == kArrangementRenderFrames);
    const auto is_valid_partition = [](std::size_t frames) {
        return frames > 0 && frames <= 128;
    };
    REQUIRE(std::all_of(fixed_partitions.begin(), fixed_partitions.end(),
                        is_valid_partition));
    REQUIRE(std::all_of(irregular_partitions.begin(), irregular_partitions.end(),
                        is_valid_partition));
    REQUIRE(irregular_partitions.size() > fixed_partitions.size());
    for (const auto size : std::array<std::size_t, 5>{1, 7, 31, 64, 127})
        REQUIRE(std::find(irregular_partitions.begin(), irregular_partitions.end(), size) !=
                irregular_partitions.end());
    for (const auto boundary :
         std::array<std::size_t, 8>{8, 32, 40, 48, 64, 128,
                                    kArrangementMeterChangeFrame,
                                    kArrangementTempoChangeFrame}) {
        CAPTURE(boundary);
        REQUIRE(partition_ends_at(irregular_partitions, boundary));
    }
    REQUIRE(fixed_partitions.back() == 128);
    REQUIRE(irregular_partitions.back() == 128);

    const auto fixed = render_arrangement(fixed_partitions);
    const auto irregular = render_arrangement(irregular_partitions);

    REQUIRE(fixed.left.size() == kArrangementRenderFrames);
    require_exact_samples(fixed.left, fixed.right);
    require_exact_samples(irregular.left, irregular.right);
    require_exact_samples(fixed.left, irregular.left);
    require_exact_samples(fixed.right, irregular.right);

    constexpr auto latency = static_cast<std::size_t>(
        TimelineMultitrackArrangementProcessor::pdc_latency_samples);
    REQUIRE(std::all_of(fixed.left.begin(), fixed.left.begin() + latency,
                        [](float sample) { return sample == 0.0f; }));
    REQUIRE(fixed.left[latency] == 2.0f);
    REQUIRE(fixed.left[32] == 0.0f);
    REQUIRE(fixed.left[latency + 32] > 0.25f);
    REQUIRE(fixed.left[latency + 32] < 0.5f);
    REQUIRE(std::count_if(fixed.left.begin(), fixed.left.end(),
                          [](float sample) { return sample != 0.0f; }) == 2);

    REQUIRE(fixed.meter_change_applied);
    REQUIRE(irregular.meter_change_applied);
    REQUIRE(fixed.automation_event_count > 0);
    REQUIRE(irregular.automation_event_count > 0);
    REQUIRE(fixed.graph_latency_samples ==
            TimelineMultitrackArrangementProcessor::pdc_latency_samples);
    REQUIRE(fixed.graph_latency_samples == irregular.graph_latency_samples);
    REQUIRE(fixed.reported_latency_samples == irregular.reported_latency_samples);

    require_same_stable_transport(fixed.terminal_transport, irregular.terminal_transport);
    REQUIRE(fixed.terminal_transport.frame_count == 128);
    REQUIRE(fixed.terminal_transport.range_count == 1);
    REQUIRE(fixed.terminal_transport.ranges[0].sample_offset == 0);
    REQUIRE(fixed.terminal_transport.ranges[0].frame_count == 128);
    REQUIRE(fixed.terminal_transport.ranges[0].timeline_sample_start ==
            timebase::SamplePosition{static_cast<std::int64_t>(kArrangementTempoChangeFrame)});
    REQUIRE(fixed.terminal_transport.ranges[0].timeline_tick_end >
            fixed.terminal_transport.ranges[0].timeline_tick_start);
    REQUIRE(fixed.terminal_transport.tempo_bpm == 132.0);
    REQUIRE(fixed.terminal_transport.meter == playback::MeterSignature{3, 4});
    REQUIRE(fixed.terminal_transport.is_playing);
}

#include "timeline_multitrack_arrangement.hpp"
#include "timeline_phase1_example_test_support.hpp"

static_assert(TimelineMultitrackArrangementProcessor::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);

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

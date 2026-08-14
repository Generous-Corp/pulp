#include "timeline_phase1_example_test_support.hpp"

namespace {

struct StandalonePartitionObservation {
    std::vector<float> left;
    std::vector<float> right;
    timebase::SamplePosition terminal_sample{};
    timebase::TickPosition terminal_tick{};
};

void require_samples_equal(const std::vector<float>& expected,
                           const std::vector<float>& actual) {
    REQUIRE(expected.size() == actual.size());
    const auto mismatch = std::mismatch(expected.begin(), expected.end(), actual.begin());
    if (mismatch.first != expected.end()) {
        const auto index = static_cast<std::size_t>(mismatch.first - expected.begin());
        CAPTURE(index, *mismatch.first, *mismatch.second);
        FAIL("partitioned render differs");
    }
}

StandalonePartitionObservation render_standalone_partitioned(
    format::ProcessorFactory factory, bool audio_player,
    const std::vector<std::size_t>& callback_pattern) {
    constexpr std::size_t kTotalFrames = 24'128;
    constexpr std::size_t kSharedFinalBlockStart = kTotalFrames - 64;
    format::StandaloneApp app(factory);
    format::StandaloneConfig config;
    config.sample_rate = 48'000.0;
    config.buffer_size = 128;
    config.input_channels = 0;
    config.output_channels = 2;
    config.persist_settings = false;
    config.route_test_signal_to_output = false;
    app.set_config(config);
    format::StandaloneRenderTestAccess::ensure_processor(app);
    format::StandaloneRenderTestAccess::prepare(app);
    REQUIRE(app.processor());

    StandalonePartitionObservation observation;
    observation.left.reserve(kTotalFrames);
    observation.right.reserve(kTotalFrames);
    std::size_t rendered = 0;
    std::size_t callback = 0;
    while (rendered < kTotalFrames) {
        const auto frames = rendered == kSharedFinalBlockStart
                                ? kTotalFrames - rendered
                                : std::min(callback_pattern[callback % callback_pattern.size()],
                                           kSharedFinalBlockStart - rendered);
        StereoBlock block(frames);
        audio::CallbackContext context;
        context.sample_rate = config.sample_rate;
        context.buffer_size = static_cast<std::uint32_t>(frames);
        context.sample_position = rendered;
        auto output = block.output();
        const auto input = block.input();
        format::StandaloneRenderTestAccess::render(app, input, output, context);
        observation.left.insert(observation.left.end(), block.left.begin(), block.left.end());
        observation.right.insert(observation.right.end(), block.right.begin(), block.right.end());
        rendered += frames;
        ++callback;
    }

    const playback::TransportSnapshot* transport = nullptr;
    if (audio_player) {
        const auto* processor = dynamic_cast<const TimelineAudioPlayerProcessor*>(app.processor());
        REQUIRE(processor);
        transport = &processor->last_transport();
    } else {
        const auto* processor =
            dynamic_cast<const TimelineStepSequencerProcessor*>(app.processor());
        REQUIRE(processor);
        transport = &processor->last_transport();
    }
    REQUIRE(transport->range_count > 0);
    const auto& terminal_range = transport->ranges[transport->range_count - 1];
    observation.terminal_sample = timebase::SamplePosition{
        terminal_range.timeline_sample_start.value + terminal_range.frame_count};
    observation.terminal_tick = terminal_range.timeline_tick_end;
    return observation;
}

} // namespace

TEST_CASE("timeline examples render deterministically through standalone callback seam") {
    struct Case {
        format::ProcessorFactory factory;
        bool audio_player;
    };
    const std::array cases{
        Case{create_validation_timeline_audio_player, true},
        Case{create_timeline_step_sequencer, false},
    };
    for (const auto& example : cases) {
        format::StandaloneApp app(example.factory);
        format::StandaloneConfig config;
        config.sample_rate = 48'000.0;
        config.buffer_size = 64;
        config.input_channels = 0;
        config.output_channels = 2;
        config.persist_settings = false;
        config.route_test_signal_to_output = false;
        app.set_config(config);
        format::StandaloneRenderTestAccess::ensure_processor(app);
        format::StandaloneRenderTestAccess::prepare(app);
        REQUIRE(app.processor());
        if (example.audio_player) {
            const auto* processor =
                dynamic_cast<const TimelineAudioPlayerProcessor*>(app.processor());
            REQUIRE(processor);
            REQUIRE(processor->engine_prepared());
        } else {
            const auto* processor =
                dynamic_cast<const TimelineStepSequencerProcessor*>(app.processor());
            REQUIRE(processor);
            REQUIRE(processor->engine_prepared());
        }

        StereoBlock block(64);
        audio::CallbackContext context;
        context.sample_rate = 48'000.0;
        context.buffer_size = 64;
        for (int warmup = 0; warmup < 2; ++warmup) {
            auto output = block.output();
            auto input = block.input();
            format::StandaloneRenderTestAccess::render(app, input, output, context);
            REQUIRE(block.energy() > 0.0);
            context.sample_position += 64;
        }
        std::size_t allocations = 1;
        {
            test::ScopedRtProcessProbe probe;
            auto output = block.output();
            auto input = block.input();
            format::StandaloneRenderTestAccess::render(app, input, output, context);
            allocations = probe.allocation_count();
        }
        REQUIRE(allocations == 0);
        REQUIRE(block.energy() > 0.0);
        REQUIRE(block.left == block.right);
    }
}

TEST_CASE("timeline standalone rendering is exact across callback partitions") {
    struct Case {
        format::ProcessorFactory factory;
        bool audio_player;
    };
    const std::array cases{
        Case{create_validation_timeline_audio_player, true},
        Case{create_timeline_step_sequencer, false},
    };

    for (const auto& example : cases) {
        const auto fixed =
            render_standalone_partitioned(example.factory, example.audio_player, {64});
        const auto irregular = render_standalone_partitioned(
            example.factory, example.audio_player, {1, 7, 31, 64, 127});

        REQUIRE(fixed.left.size() == 24'128);
        require_samples_equal(fixed.left, fixed.right);
        require_samples_equal(irregular.left, irregular.right);
        REQUIRE(std::any_of(fixed.left.begin(), fixed.left.end(),
                            [](float sample) { return sample != 0.0f; }));
        require_samples_equal(fixed.left, irregular.left);
        require_samples_equal(fixed.right, irregular.right);
        REQUIRE(fixed.terminal_sample == irregular.terminal_sample);
        REQUIRE(fixed.terminal_tick == irregular.terminal_tick);
        REQUIRE(fixed.terminal_sample == timebase::SamplePosition{24'128});
    }
}

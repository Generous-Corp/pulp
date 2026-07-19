#include "timeline_phase1_example_test_support.hpp"

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

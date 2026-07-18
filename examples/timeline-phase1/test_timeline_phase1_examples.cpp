#include "timeline_audio_player.hpp"
#include "timeline_step_sequencer.hpp"

#include "harness/scoped_rt_process_probe.hpp"

#include <pulp/format/headless.hpp>
#include <pulp/format/standalone.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <vector>

using namespace pulp;
using namespace pulp::examples::timeline_phase1;

namespace {

struct StereoBlock {
    explicit StereoBlock(std::size_t frames)
        : left(frames), right(frames), silent_left(frames), silent_right(frames),
          output_ptrs{left.data(), right.data()},
          input_ptrs{silent_left.data(), silent_right.data()} {}

    audio::BufferView<float> output() {
        return {output_ptrs.data(), output_ptrs.size(), left.size()};
    }
    audio::BufferView<const float> input() const {
        return {input_ptrs.data(), input_ptrs.size(), silent_left.size()};
    }
    double energy(std::size_t first = 0) const {
        double result = 0.0;
        for (std::size_t frame = first; frame < left.size(); ++frame)
            result += std::abs(left[frame]) + std::abs(right[frame]);
        return result;
    }

    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> silent_left;
    std::vector<float> silent_right;
    std::array<float*, 2> output_ptrs;
    std::array<const float*, 2> input_ptrs;
};

format::PrepareContext prepare_context(int maximum_block = 128) {
    return {.sample_rate = 48'000.0,
            .max_buffer_size = maximum_block,
            .input_channels = 0,
            .output_channels = 2};
}

void process_direct(format::Processor& processor, StereoBlock& block) {
    auto output = block.output();
    auto input = block.input();
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    processor.process(output, input, midi_in, midi_out, {});
}

} // namespace

static_assert(TimelineExampleEngine::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
static_assert(TimelineAudioPlayerProcessor::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
static_assert(TimelineStepSequencerProcessor::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);

TEST_CASE("timeline audio player rejects WAV beyond bounded decode limits") {
    const auto wav = make_timeline_audio_validation_wav(64);
    audio::WavDecodeLimits limits;
    limits.max_frames = 63;
    limits.max_channels = 2;
    limits.max_output_bytes = 1024;
    TimelineAudioPlayerProcessor processor(wav, limits);
    REQUIRE_FALSE(processor.source_valid());
}

TEST_CASE("timeline audio player runs through headless host with varying blocks") {
    format::HeadlessHost host(create_validation_timeline_audio_player);
    host.prepare(48'000.0, 128, 0, 2);
    REQUIRE(host.valid());
    auto* processor = host.processor_as<TimelineAudioPlayerProcessor>();
    REQUIRE(processor);
    REQUIRE(processor->source_valid());
    REQUIRE(processor->engine_prepared());

    double energy = 0.0;
    for (const std::size_t frames : {1u, 17u, 64u, 128u}) {
        StereoBlock block(frames);
        auto output = block.output();
        auto input = block.input();
        host.process(output, input);
        energy += block.energy();
        REQUIRE(processor->last_transport().frame_count == frames);
    }
    REQUIRE(energy > 0.0);
}

TEST_CASE("timeline audio player projects a loop into two exact graph ranges") {
    const auto wav = make_timeline_audio_validation_wav(512);
    TimelineAudioPlayerProcessor processor(wav);
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    REQUIRE(processor.set_loop_samples(true, 0, 256) == playback::TransportError::None);
    REQUIRE(processor.seek_samples(240) == playback::TransportError::None);
    StereoBlock block(32);
    process_direct(processor, block);
    REQUIRE(processor.last_transport().range_count == 2);
    REQUIRE(processor.last_transport().ranges[0].frame_count == 16);
    REQUIRE(processor.last_transport().ranges[1].sample_offset == 16);
    REQUIRE(block.energy() > 0.0);
}

TEST_CASE("timeline audio player graph process is allocation free after prepare") {
    const auto wav = make_timeline_audio_validation_wav(512);
    TimelineAudioPlayerProcessor processor(wav);
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    StereoBlock block(64);
    process_direct(processor, block);
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        process_direct(processor, block);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

TEST_CASE("timeline step sequencer preserves frozen grid state and renders audibly") {
    format::HeadlessHost host(create_timeline_step_sequencer);
    host.prepare(48'000.0, 128, 0, 2);
    REQUIRE(host.valid());
    auto* processor = host.processor_as<TimelineStepSequencerProcessor>();
    REQUIRE(processor);
    REQUIRE(processor->engine_prepared());

    const auto& published = processor->channel().ui_read_latest_snapshot();
    REQUIRE(published.epoch == 1);
    REQUIRE(published.patterns[0].length == 16);
    REQUIRE(published.patterns[0].lanes[0][0].enabled());

    double energy = 0.0;
    for (const std::size_t frames : {1u, 31u, 64u, 128u}) {
        StereoBlock block(frames);
        auto output = block.output();
        auto input = block.input();
        host.process(output, input);
        energy += block.energy();
        REQUIRE(processor->last_transport().frame_count == frames);
    }
    REQUIRE(energy > 0.0);
    const auto playhead = processor->channel().ui_read_playhead();
    REQUIRE(playhead.playing == 1);
}

TEST_CASE("timeline step sequencer loop flushes voices and never leaves stuck notes") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    constexpr std::int64_t kBarSamples = 96'000;
    REQUIRE(processor.seek_samples(kBarSamples - 32) == playback::TransportError::None);
    StereoBlock wrap(64);
    process_direct(processor, wrap);
    REQUIRE(processor.last_transport().range_count == 2);
    REQUIRE(wrap.energy(32) > 0.0);
    REQUIRE(processor.has_active_notes());

    REQUIRE(processor.set_playing(false) == playback::TransportError::None);
    StereoBlock stopped(64);
    process_direct(processor, stopped);
    REQUIRE_FALSE(processor.has_active_notes());
    REQUIRE(stopped.energy() == 0.0);
}

TEST_CASE("timeline step sequencer graph process is allocation free after prepare") {
    TimelineStepSequencerProcessor processor;
    processor.prepare(prepare_context());
    REQUIRE(processor.engine_prepared());
    StereoBlock block(64);
    process_direct(processor, block);
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        process_direct(processor, block);
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

TEST_CASE("timeline examples construct and prepare in the standalone host") {
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
        config.headless = true;
        config.sample_rate = 48'000.0;
        config.buffer_size = 128;
        config.input_channels = 0;
        config.output_channels = 2;
        app.set_config(config);
        if (!app.start()) {
            SUCCEED("no audio device available for standalone callback path");
            continue;
        }
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
        app.stop();
    }
}

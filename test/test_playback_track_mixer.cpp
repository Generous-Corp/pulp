#include "playback_audio_renderer_test_support.hpp"

#include <pulp/playback/track_automation_program.hpp>

namespace {

// Half a quarter at 120 BPM and 48 kHz. Named so the sample offsets below read
// as positions on the authored curve rather than as magic numbers.
constexpr std::int64_t kQuarterSamples = 24'000;

AutomationLane mixer_lane(std::uint64_t lane_id, TrackMixerParameter parameter, float first,
                          float second) {
    auto curve = take(AutomationCurve::create(
        {AutomationPoint{{lane_id + 1}, {0}, first, AutomationInterpolation::Continuous, 0.0f},
         AutomationPoint{{lane_id + 2},
                         {kTicksPerQuarter},
                         second,
                         AutomationInterpolation::Continuous,
                         0.0f}}));
    return take(AutomationLane::create({lane_id}, TrackMixerTarget{parameter}, std::move(curve)));
}

// One track holding a constant full-scale source, so any deviation the renderer
// produces is the mixer's doing and nothing else's.
struct MixerFixture {
    static constexpr std::uint64_t kAssetFrames = 64'000;

    explicit MixerFixture(TrackMixer mixer, std::vector<AutomationLane> lanes = {},
                          std::size_t channels = 1) {
        auto source = std::make_shared<audio::AudioFileData>();
        source->sample_rate = 48'000;
        source->channels.assign(channels, std::vector<float>(kAssetFrames, 1.0f));
        auto clip = absolute_media_clip(100, 0, kAssetFrames, 3, 0, kAssetFrames);
        auto track = take(Track::create(TrackInput{.id = {10},
                                                   .name = "mixed",
                                                   .clips = {clip},
                                                   .automation_lanes = std::move(lanes),
                                                   .mixer = mixer}));
        auto project = project_with_tracks({track}, {{3, "tone", kAssetFrames, {48'000, 1}}});
        compiled = std::make_unique<CompiledFixture>(
            std::move(project), map_120(), pool({{3, std::move(source)}}));
    }

    float render_first(std::int64_t sample_start, std::size_t channel = 0,
                       std::size_t channels = 1) {
        auto program = compiled->store.read();
        Output output(channels, 8);
        REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 8, sample_start),
                                                  output.view()) == AudioRenderStatus::Rendered);
        return output.storage[channel][0];
    }

    std::unique_ptr<CompiledFixture> compiled;
};

} // namespace

TEST_CASE("a static track gain scales the rendered output", "[playback][mixer]") {
    MixerFixture unity(TrackMixer{});
    MixerFixture halved(TrackMixer{0.5f, 0.0f});
    REQUIRE_THAT(unity.render_first(0), WithinAbs(1.0f, 1e-7f));
    REQUIRE_THAT(halved.render_first(0), WithinAbs(0.5f, 1e-7f));

    // A silenced track must reach exact zero, not merely a small number: a fader
    // pulled all the way down is a mute, and "almost silent" is a defect.
    MixerFixture silenced(TrackMixer{0.0f, 0.0f});
    REQUIRE(silenced.render_first(0) == 0.0f);
}

TEST_CASE("track pan attenuates one side and leaves centre at unity", "[playback][mixer]") {
    MixerFixture centred(TrackMixer{}, {}, 2);
    REQUIRE_THAT(centred.render_first(0, 0, 2), WithinAbs(1.0f, 1e-7f));
    REQUIRE_THAT(centred.render_first(0, 1, 2), WithinAbs(1.0f, 1e-7f));

    MixerFixture hard_left(TrackMixer{1.0f, -1.0f}, {}, 2);
    REQUIRE_THAT(hard_left.render_first(0, 0, 2), WithinAbs(1.0f, 1e-7f));
    REQUIRE(hard_left.render_first(0, 1, 2) == 0.0f);

    MixerFixture hard_right(TrackMixer{1.0f, 1.0f}, {}, 2);
    REQUIRE(hard_right.render_first(0, 0, 2) == 0.0f);
    REQUIRE_THAT(hard_right.render_first(0, 1, 2), WithinAbs(1.0f, 1e-7f));

    // Half right leaves the right side untouched and pulls the left down by the
    // same fraction, so a pan can never make a track louder than its fader.
    MixerFixture half_right(TrackMixer{1.0f, 0.5f}, {}, 2);
    REQUIRE_THAT(half_right.render_first(0, 0, 2), WithinAbs(0.5f, 1e-7f));
    REQUIRE_THAT(half_right.render_first(0, 1, 2), WithinAbs(1.0f, 1e-7f));
}

TEST_CASE("automation targeting track gain reaches the audio output", "[playback][mixer]") {
    // The point of this case is that the assertion is on rendered samples, not on
    // the document: a lane that exists but never moves a sample would pass every
    // model-level check and still be worthless.
    MixerFixture automated(TrackMixer{1.0f, 0.0f},
                           {mixer_lane(40, TrackMixerParameter::Gain, 0.25f, 0.75f)});
    REQUIRE_THAT(automated.render_first(0), WithinAbs(0.25f, 1e-4f));
    REQUIRE_THAT(automated.render_first(kQuarterSamples / 2), WithinAbs(0.5f, 1e-3f));
    REQUIRE_THAT(automated.render_first(kQuarterSamples), WithinAbs(0.75f, 1e-4f));
    // Past the last authored point the curve holds its final value rather than
    // falling back to the static gain.
    REQUIRE_THAT(automated.render_first(kQuarterSamples * 2), WithinAbs(0.75f, 1e-4f));

    // The lane supersedes the authored constant outright; if the two multiplied,
    // this would render at half the value the curve asks for.
    MixerFixture over_constant(TrackMixer{0.5f, 0.0f},
                               {mixer_lane(40, TrackMixerParameter::Gain, 0.25f, 0.75f)});
    REQUIRE_THAT(over_constant.render_first(0), WithinAbs(0.25f, 1e-4f));
}

TEST_CASE("a gain curve moves within one rendered block", "[playback][mixer]") {
    // Evaluating the curve once per block instead of per sample would leave every
    // sample in the block equal, which is exactly what this rejects.
    MixerFixture automated(TrackMixer{},
                           {mixer_lane(40, TrackMixerParameter::Gain, 0.0f, 1.0f)});
    auto program = automated.compiled->store.read();
    Output output(1, 512);
    REQUIRE(ArrangementAudioRenderer::process(*program, snapshot(*program, 512, 0),
                                              output.view()) == AudioRenderStatus::Rendered);
    REQUIRE(output.storage[0][0] < output.storage[0][255]);
    REQUIRE(output.storage[0][255] < output.storage[0][511]);
    REQUIRE_THAT(output.storage[0][511],
                 WithinAbs(511.0f / static_cast<float>(kQuarterSamples), 1e-4f));
}

TEST_CASE("host-mapped mixer automation preserves fractional document ticks",
          "[playback][mixer][host-tempo]") {
    constexpr std::uint64_t kFrames = 32'000;
    auto source = std::make_shared<audio::AudioFileData>();
    source->sample_rate = 48'000;
    source->channels = {std::vector<float>(kFrames, 1.0f)};
    auto clip = musical_media_clip(100, 0, kTicksPerQuarter, 3, kFrames);
    auto track = take(Track::create(TrackInput{
        .id = {10},
        .name = "fractional mixer",
        .clips = {clip},
        .automation_lanes =
            {
                mixer_lane(40, TrackMixerParameter::Gain, 0.0f, 1.0f),
            },
    }));
    auto project = project_with_tracks({track}, {{3, "tone", kFrames, {48'000, 1}}});
    CompiledFixture compiled(std::move(project), map_120(), pool({{3, std::move(source)}}));
    auto program = compiled.store.read();

    auto mapped = snapshot(*program, 4);
    auto& range = mapped.ranges[0];
    range.timeline_tick_start = {0};
    range.timeline_tick_end = {1};
    range.host_tick_start = 0.025;
    range.host_tick_end = 0.105;
    range.has_precise_host_ticks = true;
    range.host_beat_mapping = true;
    Output output(1, 4);
    REQUIRE(ArrangementAudioRenderer::process(*program, mapped, output.view()) ==
            AudioRenderStatus::Rendered);

    for (std::size_t frame = 0; frame < 4; ++frame) {
        const auto tick = 0.025 + 0.08 * static_cast<double>(frame) / 4.0;
        REQUIRE_THAT(output.storage[0][frame],
                     WithinAbs(static_cast<float>(tick / kTicksPerQuarter), 1.0e-7f));
    }
    REQUIRE(output.storage[0][0] < output.storage[0][1]);
    REQUIRE(output.storage[0][1] < output.storage[0][2]);
    REQUIRE(output.storage[0][2] < output.storage[0][3]);
}

TEST_CASE("automation targeting track pan reaches the audio output", "[playback][mixer]") {
    MixerFixture automated(TrackMixer{}, {mixer_lane(40, TrackMixerParameter::Pan, -1.0f, 1.0f)},
                           2);
    REQUIRE_THAT(automated.render_first(0, 0, 2), WithinAbs(1.0f, 1e-4f));
    REQUIRE_THAT(automated.render_first(0, 1, 2), WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(automated.render_first(kQuarterSamples, 0, 2), WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(automated.render_first(kQuarterSamples, 1, 2), WithinAbs(1.0f, 1e-4f));
}

TEST_CASE("mixer lanes never reach device parameter delivery", "[playback][mixer]") {
    // A mixer lane addresses no device, so the compiled device grouping must not
    // grow one for it; otherwise delivery would try to find a plugin to send it
    // to and refuse the whole track.
    MixerFixture automated(TrackMixer{}, {mixer_lane(40, TrackMixerParameter::Gain, 0.0f, 1.0f)});
    auto program = automated.compiled->store.read();
    const auto& track = *program->tracks()[0];
    REQUIRE(track.automation_program() != nullptr);
    REQUIRE(track.automation_program()->programs().size() == 1);
    REQUIRE(track.automation_program()->programs()[0]->device_target() == nullptr);
    REQUIRE(track.ordered_device_placement_ids().empty());
    REQUIRE(track.mixer().gain_automation != nullptr);
    REQUIRE(track.mixer().pan_automation == nullptr);
}

TEST_CASE("rendering an automated mixer allocates nothing", "[playback][mixer][rt-safety]") {
    MixerFixture automated(TrackMixer{0.5f, 0.25f},
                           {mixer_lane(40, TrackMixerParameter::Gain, 0.25f, 0.75f),
                            mixer_lane(50, TrackMixerParameter::Pan, -1.0f, 1.0f)},
                           2);
    auto program = automated.compiled->store.read();
    Output output(2, 256);
    const auto state = snapshot(*program, 256, 0);
    {
        test::RtAllocationProbe probe;
        REQUIRE(ArrangementAudioRenderer::process(*program, state, output.view()) ==
                AudioRenderStatus::Rendered);
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("post-device mixer automation stays parked while transport is stopped",
          "[playback][mixer][transport]") {
    MixerFixture automated(TrackMixer{},
                           {mixer_lane(40, TrackMixerParameter::Gain, 0.0f, 1.0f)});
    PlaybackProgramBlockLatch latch;
    const auto block = latch.begin_block(automated.compiled->store);
    REQUIRE(block);
    TrackMixerTrackRenderer renderer({10});
    audio::Buffer<float> input(1, 8);
    std::fill(input.view().channel(0).begin(), input.view().channel(0).end(), 1.0f);
    const auto& const_input = input;
    Output output(1, 8);
    auto stopped = snapshot(*block.program(), 8, kQuarterSamples / 2);
    stopped.is_playing = false;

    REQUIRE(renderer.process(block, stopped, output.view(), const_input.view()) ==
            AudioRenderStatus::Rendered);
    REQUIRE_THAT(output.storage[0].front(), WithinAbs(0.5f, 1.0e-3f));
    REQUIRE(std::all_of(output.storage[0].begin(), output.storage[0].end(),
                        [&](float sample) { return sample == output.storage[0].front(); }));
}

TEST_CASE("an out-of-range gain curve is bounded at the render", "[playback][mixer]") {
    // An automation curve is authored point by point and is not range checked at
    // insert time, so the render is the only place that can hold a lane to the
    // same range the document accepts for a static fader.
    MixerFixture excessive(
        TrackMixer{}, {mixer_lane(40, TrackMixerParameter::Gain, 1'000.0f, 1'000.0f)});
    REQUIRE_THAT(excessive.render_first(0), WithinAbs(64.0f, 1e-4f));

    MixerFixture negative(TrackMixer{}, {mixer_lane(40, TrackMixerParameter::Gain, -5.0f, -5.0f)});
    REQUIRE(negative.render_first(0) == 0.0f);

    // A pan curve past hard left must not invert the opposite side into a
    // negative multiplier, which would flip that channel's polarity.
    MixerFixture over_panned(TrackMixer{},
                             {mixer_lane(40, TrackMixerParameter::Pan, -4.0f, -4.0f)}, 2);
    REQUIRE_THAT(over_panned.render_first(0, 0, 2), WithinAbs(1.0f, 1e-4f));
    REQUIRE(over_panned.render_first(0, 1, 2) == 0.0f);
}

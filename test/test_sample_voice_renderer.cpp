#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/sample_voice_renderer.hpp>
#include <pulp/audio/loop_reader.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <vector>

#include <pulp/signal/interpolator.hpp>

using Catch::Matchers::WithinAbs;
using pulp::audio::AhdsrEnvelope;
using pulp::audio::AhdsrEnvelopeConfig;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::LoopInterpolationMode;
using pulp::audio::LoopPlaybackMode;
using pulp::audio::LoopRegion;
using pulp::audio::LoopReader;
using pulp::audio::PublishedSampleStore;
using pulp::audio::PublishedSampleStoreConfig;
using pulp::audio::SamplePool;
using pulp::audio::SamplePoolEntry;
using pulp::audio::SamplePoolResolution;
using pulp::audio::SampleVoiceRenderer;
using pulp::audio::SampleVoiceRenderOptions;
using pulp::audio::SampleVoiceRenderState;

namespace {

void prepare_store(PublishedSampleStore& store,
                   std::span<const float> samples,
                   double sample_rate = 48000.0) {
    REQUIRE(store.prepare(PublishedSampleStoreConfig{
        .slot_count = 2,
        .max_channels = 1,
        .max_frames_per_slot = samples.size(),
    }));
    REQUIRE(store.load_mono(samples.data(),
                            static_cast<int>(samples.size()),
                            sample_rate));
}

void prepare_stereo_store(PublishedSampleStore& store,
                          std::span<const float> interleaved) {
    REQUIRE(interleaved.size() % 2 == 0);
    const auto frames = interleaved.size() / 2;
    REQUIRE(store.prepare(PublishedSampleStoreConfig{
        .slot_count = 2,
        .max_channels = 2,
        .max_frames_per_slot = frames,
    }));
    REQUIRE(store.load_interleaved_stereo(interleaved.data(),
                                          static_cast<int>(frames),
                                          48000.0));
}

SamplePoolResolution resolve_sample(PublishedSampleStore& store) {
    SamplePool pool;
    std::array<SamplePoolEntry, 1> entries{SamplePoolEntry{
        .sample_id = 1,
        .store = &store,
        .view = store.read_published_view(),
    }};
    REQUIRE(pool.configure(entries));
    auto resolution = pool.resolve(1);
    REQUIRE(resolution.valid);
    return resolution;
}

LoopRegion playback_region(std::uint64_t start,
                           std::uint64_t end,
                           LoopPlaybackMode mode,
                           LoopInterpolationMode interpolation =
                               LoopInterpolationMode::None) {
    LoopRegion region;
    region.start_frame = start;
    region.end_frame = end;
    region.source_sample_rate = 48000.0;
    region.playback_mode = mode;
    region.interpolation = interpolation;
    return region;
}

}  // namespace

TEST_CASE("SampleVoiceRenderer renders mono samples to multiple output channels",
          "[audio][sampler][voice-render]") {
    std::array<float, 4> samples{0.0f, 1.0f, 0.0f, -1.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(2, 4);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 1.0,
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        4,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 4);
    REQUIRE(result.finished);
    REQUIRE_FALSE(state.active);
    for (std::size_t channel = 0; channel < output.num_channels(); ++channel) {
        REQUIRE_THAT(output.channel(channel)[0], WithinAbs(0.0f, 1.0e-6f));
        REQUIRE_THAT(output.channel(channel)[1], WithinAbs(1.0f, 1.0e-6f));
        REQUIRE_THAT(output.channel(channel)[2], WithinAbs(0.0f, 1.0e-6f));
        REQUIRE_THAT(output.channel(channel)[3], WithinAbs(-1.0f, 1.0e-6f));
    }
}

TEST_CASE("SampleVoiceRenderer leaves extra multichannel outputs silent",
          "[audio][sampler][voice-render]") {
    std::array<float, 4> interleaved{
        1.0f, 10.0f,
        2.0f, 20.0f,
    };
    PublishedSampleStore store;
    prepare_stereo_store(store, interleaved);

    Buffer<float> output(4, 2);
    std::array<const float*, 2> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 1.0,
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        2,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 2);
    REQUIRE(result.finished);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(2.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(1)[0], WithinAbs(10.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(1)[1], WithinAbs(20.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(2)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(2)[1], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(3)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(3)[1], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer linearly interpolates fractional playback",
          "[audio][sampler][voice-render]") {
    std::array<float, 3> samples{0.0f, 1.0f, 0.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 4);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 0.5,
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        4,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 4);
    REQUIRE_FALSE(result.finished);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(0.5f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[2], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[3], WithinAbs(0.5f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer applies source-to-host sample-rate step",
          "[audio][sampler][voice-render][sample-rate]") {
    std::array<float, 3> samples{0.0f, 1.0f, 0.0f};
    PublishedSampleStore store;
    prepare_store(store, samples, 24000.0);

    Buffer<float> output(1, 4);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .host_sample_rate = 48000.0,
        .playback_rate = 1.0,
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        4,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 4);
    REQUIRE_FALSE(result.finished);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(0.5f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[2], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[3], WithinAbs(0.5f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer applies interpolation policy to synthesized regions",
          "[audio][sampler][voice-render][loop]") {
    std::array<float, 2> samples{0.0f, 1.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 3);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 0.5,
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        3,
        source_channels,
        SampleVoiceRenderOptions{
            .accumulate = false,
            .interpolation = LoopInterpolationMode::None,
        });

    REQUIRE(result.rendered_frames == 3);
    REQUIRE_FALSE(result.finished);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[2], WithinAbs(1.0f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer explicit region interpolation overrides options",
          "[audio][sampler][voice-render][loop]") {
    std::array<float, 2> samples{0.0f, 10.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 1);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .position_frames = 0.5,
        .use_playback_region = true,
        .playback_region = playback_region(0, 2,
                                           LoopPlaybackMode::OneShot,
                                           LoopInterpolationMode::Linear),
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        1,
        source_channels,
        SampleVoiceRenderOptions{
            .accumulate = false,
            .interpolation = LoopInterpolationMode::None,
        });

    REQUIRE(result.rendered_frames == 1);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(5.0f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer renders explicit one-shot subregions",
          "[audio][sampler][voice-render][loop]") {
    std::array<float, 4> samples{10.0f, 20.0f, 30.0f, 40.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 3);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .position_frames = 1.0,
        .use_playback_region = true,
        .playback_region = playback_region(1, 3, LoopPlaybackMode::OneShot),
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        3,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 2);
    REQUIRE(result.silent_frames == 1);
    REQUIRE(result.finished);
    REQUIRE_FALSE(state.active);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(20.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(30.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[2], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer does not auto-seek explicit regions",
          "[audio][sampler][voice-render][loop]") {
    std::array<float, 4> samples{10.0f, 20.0f, 30.0f, 40.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 2);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .position_frames = 0.0,
        .use_playback_region = true,
        .playback_region = playback_region(1, 3, LoopPlaybackMode::OneShot),
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        2,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 0);
    REQUIRE(result.silent_frames == 2);
    REQUIRE(result.finished);
    REQUIRE_FALSE(state.active);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer fills missing playback-region sample rate",
          "[audio][sampler][voice-render][loop]") {
    std::array<float, 2> samples{0.25f, 0.5f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    auto region = playback_region(0, 2, LoopPlaybackMode::OneShot);
    region.source_sample_rate = 0.0;

    Buffer<float> output(1, 2);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .use_playback_region = true,
        .playback_region = region,
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        2,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 2);
    REQUIRE(result.finished);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(0.5f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer finishes invalid explicit regions silently",
          "[audio][sampler][voice-render][loop][edge]") {
    std::array<float, 2> samples{0.25f, 0.5f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 2);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .use_playback_region = true,
        .playback_region = playback_region(0, 8, LoopPlaybackMode::Forward),
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        2,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 0);
    REQUIRE(result.silent_frames == 2);
    REQUIRE(result.finished);
    REQUIRE_FALSE(state.active);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer loops explicit forward regions",
          "[audio][sampler][voice-render][loop]") {
    std::array<float, 4> samples{10.0f, 20.0f, 30.0f, 40.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 5);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .position_frames = 1.0,
        .use_playback_region = true,
        .playback_region = playback_region(1, 3, LoopPlaybackMode::Forward),
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        5,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 5);
    REQUIRE_FALSE(result.finished);
    REQUIRE(state.active);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(20.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(30.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[2], WithinAbs(20.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[3], WithinAbs(30.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[4], WithinAbs(20.0f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer reverse loops remain valid across blocks",
          "[audio][sampler][voice-render][loop]") {
    std::array<float, 3> samples{0.0f, 1.0f, 2.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 4);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .position_frames = 0.0,
        .use_playback_region = true,
        .playback_region = playback_region(0, 3, LoopPlaybackMode::Reverse),
    };

    auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        4,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 4);
    REQUIRE_FALSE(result.finished);
    REQUIRE(state.active);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(2.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[2], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[3], WithinAbs(0.0f, 1.0e-6f));

    Buffer<float> next_output(1, 3);
    result = SampleVoiceRenderer::render(
        state,
        next_output.view(),
        3,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 3);
    REQUIRE_FALSE(result.finished);
    REQUIRE(state.active);
    REQUIRE_THAT(next_output.channel(0)[0], WithinAbs(2.0f, 1.0e-6f));
    REQUIRE_THAT(next_output.channel(0)[1], WithinAbs(1.0f, 1.0e-6f));
    REQUIRE_THAT(next_output.channel(0)[2], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer reports finished voices and leaves silence",
          "[audio][sampler][voice-render]") {
    std::array<float, 2> samples{0.25f, 0.5f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 4);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 1.0,
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        4,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 2);
    REQUIRE(result.silent_frames == 2);
    REQUIRE(result.finished);
    REQUIRE_FALSE(state.active);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(0.5f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[2], WithinAbs(0.0f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[3], WithinAbs(0.0f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer applies optional envelope gain",
          "[audio][sampler][voice-render][envelope]") {
    std::array<float, 4> samples{1.0f, 1.0f, 1.0f, 1.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    AhdsrEnvelope envelope;
    REQUIRE(envelope.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 4.0,
        .attack_seconds = 0.5,
        .hold_seconds = 0.0,
        .decay_seconds = 0.0,
        .sustain_level = 1.0,
        .release_seconds = 0.0,
    }));
    envelope.note_on();

    Buffer<float> output(1, 3);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 1.0,
        .gain = 0.5f,
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        3,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false, .envelope = &envelope});

    REQUIRE(result.rendered_frames == 3);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(0.5f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[2], WithinAbs(0.5f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer fades stolen voices without a discontinuity",
          "[audio][sampler][voice-render][steal]") {
    std::array<float, 128> samples{};
    samples.fill(1.0f);
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 64);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 1.0,
    };
    SampleVoiceRenderer::begin_fade_out(state, 64);

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        64,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 64);
    REQUIRE(result.finished);
    REQUIRE_FALSE(state.active);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.9992752f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[63], WithinAbs(0.0f, 1.0e-6f));
    for (std::size_t i = 1; i < 64; ++i) {
        REQUIRE(output.channel(0)[i] <= output.channel(0)[i - 1]);
        REQUIRE(std::abs(output.channel(0)[i] - output.channel(0)[i - 1]) < 0.025f);
    }
}

TEST_CASE("SampleVoiceRenderer matches LoopReader reference for cubic loops",
          "[audio][sampler][voice-render][loop][null]") {
    std::array<float, 5> samples{0.0f, 0.2f, 0.8f, 0.1f, -0.3f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 12);
    std::array<const float*, 1> source_channels{};
    auto sample = resolve_sample(store);
    const LoopRegion region = playback_region(1,
                                              5,
                                              LoopPlaybackMode::Forward,
                                              LoopInterpolationMode::Cubic);
    SampleVoiceRenderState state{
        .active = true,
        .sample = sample,
        .position_frames = 1.25,
        .playback_rate = 0.375,
        .use_playback_region = true,
        .playback_region = region,
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        12,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});

    REQUIRE(result.rendered_frames == 12);
    auto view = sample.store->read_published_view();
    REQUIRE(view.valid);
    BufferView<const float> source_view(
        source_channels.data(),
        1,
        static_cast<std::size_t>(view.num_frames));
    double position = 1.25;
    for (std::size_t frame = 0; frame < 12; ++frame) {
        const auto expected = LoopReader::read_validated(source_view,
                                                         region,
                                                         0,
                                                         position);
        REQUIRE_THAT(output.channel(0)[frame], WithinAbs(expected, 1.0e-6f));
        position += 0.375;
        position = LoopReader::normalize_position(region, position);
    }
}

TEST_CASE("SampleVoiceRenderer accumulates or overwrites by option",
          "[audio][sampler][voice-render]") {
    std::array<float, 2> samples{0.25f, 0.25f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 2);
    output.channel(0)[0] = 1.0f;
    output.channel(0)[1] = 1.0f;

    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 1.0,
    };
    SampleVoiceRenderer::render(state, output.view(), 2, source_channels);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(1.25f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(1.25f, 1.0e-6f));

    state.active = true;
    state.position_frames = 0.0;
    SampleVoiceRenderer::render(
        state,
        output.view(),
        2,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.25f, 1.0e-6f));
    REQUIRE_THAT(output.channel(0)[1], WithinAbs(0.25f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer finishes active voices that cannot render",
          "[audio][sampler][voice-render][edge]") {
    std::array<float, 2> samples{0.25f, 0.5f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 2);
    std::array<const float*, 1> source_channels{};

    SampleVoiceRenderState nan_position{
        .active = true,
        .sample = resolve_sample(store),
        .position_frames = std::nan(""),
    };
    auto result = SampleVoiceRenderer::render(
        nan_position,
        output.view(),
        2,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});
    REQUIRE(result.finished);
    REQUIRE_FALSE(nan_position.active);
    REQUIRE(result.silent_frames == 2);

    SampleVoiceRenderState undersized_scratch{
        .active = true,
        .sample = resolve_sample(store),
    };
    std::array<const float*, 0> no_channels{};
    result = SampleVoiceRenderer::render(
        undersized_scratch,
        output.view(),
        2,
        no_channels,
        SampleVoiceRenderOptions{.accumulate = false});
    REQUIRE(result.finished);
    REQUIRE_FALSE(undersized_scratch.active);
    REQUIRE(result.silent_frames == 2);

    SampleVoiceRenderState invalid_sample{.active = true};
    result = SampleVoiceRenderer::render(
        invalid_sample,
        output.view(),
        2,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false});
    REQUIRE(result.finished);
    REQUIRE_FALSE(invalid_sample.active);
    REQUIRE(result.silent_frames == 2);
}

TEST_CASE("SampleVoiceRenderer finishes immediately with an inactive envelope",
          "[audio][sampler][voice-render][envelope]") {
    std::array<float, 2> samples{1.0f, 1.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    AhdsrEnvelope envelope;
    REQUIRE(envelope.prepare(AhdsrEnvelopeConfig{}));

    Buffer<float> output(1, 2);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
    };

    const auto result = SampleVoiceRenderer::render(
        state,
        output.view(),
        2,
        source_channels,
        SampleVoiceRenderOptions{.accumulate = false, .envelope = &envelope});

    REQUIRE(result.finished);
    REQUIRE(result.rendered_frames == 0);
    REQUIRE(result.silent_frames == 2);
    REQUIRE_FALSE(state.active);
    REQUIRE_THAT(state.position_frames, WithinAbs(0.0, 1.0e-12));
}

TEST_CASE("SampleVoiceRenderer hot path does not allocate",
          "[audio][sampler][voice-render][rt]") {
    std::array<float, 8> samples{0.0f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 8);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 1.0,
    };

    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        SampleVoiceRenderer::render(
            state,
            output.view(),
            8,
            source_channels,
            SampleVoiceRenderOptions{.accumulate = false});
    }

    REQUIRE_FALSE(state.active);
    REQUIRE_THAT(output.channel(0)[7], WithinAbs(0.7f, 1.0e-6f));
}

TEST_CASE("SampleVoiceRenderer envelope hot path does not allocate",
          "[audio][sampler][voice-render][rt][envelope]") {
    std::array<float, 8> samples{1.0f, 1.0f, 1.0f, 1.0f,
                                 1.0f, 1.0f, 1.0f, 1.0f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    AhdsrEnvelope envelope;
    REQUIRE(envelope.prepare(AhdsrEnvelopeConfig{
        .sample_rate = 48000.0,
        .attack_seconds = 0.001,
        .hold_seconds = 0.0,
        .decay_seconds = 0.0,
        .sustain_level = 1.0,
        .release_seconds = 0.001,
    }));
    envelope.note_on();

    Buffer<float> output(1, 4);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .playback_rate = 1.0,
    };

    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        SampleVoiceRenderer::render(
            state,
            output.view(),
            4,
            source_channels,
            SampleVoiceRenderOptions{.accumulate = false, .envelope = &envelope});
    }

    REQUIRE(state.active);
}

TEST_CASE("SampleVoiceRenderer explicit loop region hot path does not allocate",
          "[audio][sampler][voice-render][rt][loop]") {
    std::array<float, 4> samples{0.0f, 0.1f, 0.2f, 0.3f};
    PublishedSampleStore store;
    prepare_store(store, samples);

    Buffer<float> output(1, 8);
    std::array<const float*, 1> source_channels{};
    SampleVoiceRenderState state{
        .active = true,
        .sample = resolve_sample(store),
        .position_frames = 1.0,
        .use_playback_region = true,
        .playback_region = playback_region(1, 4,
                                           LoopPlaybackMode::Forward,
                                           LoopInterpolationMode::Cubic),
    };

    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        SampleVoiceRenderer::render(
            state,
            output.view(),
            8,
            source_channels,
            SampleVoiceRenderOptions{.accumulate = false});
    }

    REQUIRE(state.active);
}

// ---------------------------------------------------------------------------
// Microbenchmark + null test for the block-kernel restructure (PF-1).
//
// Baseline: a faithful copy of the PRE-refactor inner loop — a sample-at-a-time
// out-of-line read (marked noinline, switch-on-interpolation per sample, per-tap
// clamp/wrap) exactly as the renderer's fast path used to call. New path:
// SampleVoiceRenderer::render, whose read is the inlined, mode-specialized block
// kernel with an interior fast path. The reference reproduces the identical
// float math, so the two outputs must be BIT-IDENTICAL (the null test); the
// timing ratio then isolates the block-kernel speedup. Hidden ([.]) so it never
// runs in the normal suite — invoke with the [benchmark] tag.
namespace {

#if defined(__GNUC__) || defined(__clang__)
#define PULP_BENCH_NOINLINE __attribute__((noinline))
#else
#define PULP_BENCH_NOINLINE
#endif

std::uint64_t bench_wrap_index(const LoopRegion& region, long long frame) noexcept {
    const auto length = static_cast<long long>(region.end_frame - region.start_frame);
    if (length <= 0) return region.start_frame;
    auto relative = frame - static_cast<long long>(region.start_frame);
    relative %= length;
    if (relative < 0) relative += length;
    return region.start_frame + static_cast<std::uint64_t>(relative);
}

float bench_sample_at(const float* source, std::uint64_t source_frames,
                      const LoopRegion& region, long long frame) noexcept {
    if (source == nullptr || source_frames == 0) return 0.0f;
    std::uint64_t index = 0;
    if (region.playback_mode == LoopPlaybackMode::OneShot) {
        const auto source_last = static_cast<long long>(source_frames - 1);
        const auto lo = static_cast<long long>(region.start_frame);
        const auto hi = std::min(static_cast<long long>(region.end_frame - 1), source_last);
        index = static_cast<std::uint64_t>(std::clamp(frame, lo, hi));
    } else {
        index = bench_wrap_index(region, frame);
    }
    return index < source_frames ? source[index] : 0.0f;
}

// Out-of-line per-sample read: the pre-refactor structure.
PULP_BENCH_NOINLINE float bench_read_scalar(const float* source,
                                            std::uint64_t source_frames,
                                            const LoopRegion& region,
                                            double position) noexcept {
    if (source == nullptr) return 0.0f;
    if (region.playback_mode == LoopPlaybackMode::OneShot &&
        (position < static_cast<double>(region.start_frame) ||
         position >= static_cast<double>(region.end_frame))) {
        return 0.0f;
    }
    const auto normalized = LoopReader::normalize_position(region, position);
    const auto base = static_cast<long long>(std::floor(normalized));
    const auto frac = static_cast<float>(normalized - static_cast<double>(base));
    switch (region.interpolation) {
        case LoopInterpolationMode::None:
            return bench_sample_at(source, source_frames, region, base);
        case LoopInterpolationMode::Linear: {
            const auto y0 = bench_sample_at(source, source_frames, region, base);
            const auto y1 = bench_sample_at(source, source_frames, region, base + 1);
            return pulp::signal::Interpolator::linear(frac, y0, y1);
        }
        case LoopInterpolationMode::Cubic: {
            const auto ym1 = bench_sample_at(source, source_frames, region, base - 1);
            const auto y0 = bench_sample_at(source, source_frames, region, base);
            const auto y1 = bench_sample_at(source, source_frames, region, base + 1);
            const auto y2 = bench_sample_at(source, source_frames, region, base + 2);
            return pulp::signal::Interpolator::hermite(frac, ym1, y0, y1, y2);
        }
    }
    return 0.0f;
}

// Reference renderer mirroring the pre-refactor fast path (no envelope/fade).
void bench_reference_render(SampleVoiceRenderState& state,
                            BufferView<float> destination,
                            std::uint64_t frames,
                            std::span<const float*> channel_scratch) noexcept {
    const auto source_channels =
        static_cast<std::size_t>(state.sample.view.num_channels);
    const auto source_frames = state.sample.view.num_frames;
    const auto& region = state.playback_region;
    const double source_sr = region.source_sample_rate;
    const double rate_ratio = source_sr / state.host_sample_rate;
    double step = state.playback_rate * rate_ratio;
    if (region.playback_mode == LoopPlaybackMode::Reverse) step = -step;

    for (std::size_t ch = 0; ch < destination.num_channels(); ++ch) {
        const std::size_t src_ch = ch < source_channels ? ch
                                   : (source_channels == 1 ? 0 : source_channels);
        if (src_ch >= source_channels) continue;
        const float* source = channel_scratch[src_ch];
        float* out = destination.channel_ptr(ch);
        double position = state.position_frames;
        for (std::uint64_t f = 0; f < frames; ++f) {
            out[f] += bench_read_scalar(source, source_frames, region, position) *
                      state.gain;
            position += step;
            if (region.playback_mode != LoopPlaybackMode::OneShot) {
                position = LoopReader::normalize_position(region, position);
            }
        }
    }
    // Advance play head like the real renderer.
    for (std::uint64_t f = 0; f < frames; ++f) {
        state.position_frames += step;
        if (region.playback_mode != LoopPlaybackMode::OneShot) {
            state.position_frames =
                LoopReader::normalize_position(region, state.position_frames);
        }
    }
}

template <typename Fn>
double bench_median_us(Fn&& fn, int iters) {
    std::vector<double> ts;
    ts.reserve(static_cast<std::size_t>(iters));
    for (int i = 0; i < 3; ++i) fn();  // warmup
    for (int i = 0; i < iters; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        ts.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::sort(ts.begin(), ts.end());
    return ts[ts.size() / 2];
}

}  // namespace

TEST_CASE("SampleVoiceRenderer block kernel matches the scalar path and is faster",
          "[audio][sampler][voice-render][benchmark][.]") {
    // Stereo cubic looping sustain — the dominant sampler regime PF-1 targets.
    constexpr std::size_t kFrames = 512;
    constexpr std::size_t kVoices = 32;
    constexpr std::size_t kSourceFrames = 4096;

    std::vector<float> interleaved(kSourceFrames * 2);
    for (std::size_t i = 0; i < kSourceFrames; ++i) {
        interleaved[2 * i] = std::sin(0.013f * static_cast<float>(i));
        interleaved[2 * i + 1] = std::sin(0.017f * static_cast<float>(i) + 0.5f);
    }
    PublishedSampleStore store;
    prepare_stereo_store(store, interleaved);
    auto sample = resolve_sample(store);

    // Resolve the borrowed source channel pointers once; both paths read them.
    std::array<const float*, 2> scratch{};
    REQUIRE(SamplePool::populate_channel_ptrs(sample, scratch.data(), scratch.size()));

    auto make_state = [&](const LoopRegion& region, double phase) {
        SampleVoiceRenderState s{};
        s.active = true;
        s.sample = sample;
        s.position_frames = phase;
        s.host_sample_rate = 48000.0;
        s.playback_rate = 1.0009;  // fractional step -> exercises interpolation
        s.gain = 0.5f;
        s.use_playback_region = true;
        s.playback_region = region;
        return s;
    };

    Buffer<float> out_new(2, kFrames);
    Buffer<float> out_ref(2, kFrames);
    Buffer<float> acc(2, kFrames);

    struct Scenario {
        const char* name;
        LoopPlaybackMode mode;
        LoopInterpolationMode interp;
    };
    const Scenario scenarios[] = {
        {"OneShot None  ", LoopPlaybackMode::OneShot, LoopInterpolationMode::None},
        {"OneShot Linear", LoopPlaybackMode::OneShot, LoopInterpolationMode::Linear},
        {"OneShot Cubic ", LoopPlaybackMode::OneShot, LoopInterpolationMode::Cubic},
        {"Forward Linear", LoopPlaybackMode::Forward, LoopInterpolationMode::Linear},
        {"Forward Cubic ", LoopPlaybackMode::Forward, LoopInterpolationMode::Cubic},
    };

    std::printf("\n[voice-render bench] %zu voices x %zu frames, stereo, "
                "scalar-per-sample vs inlined block kernel\n", kVoices, kFrames);

    for (const auto& sc : scenarios) {
        // OneShot must not run off the end of the source within the block, so a
        // OneShot voice's start phase is bounded well inside the region.
        const LoopRegion region = playback_region(0, kSourceFrames, sc.mode, sc.interp);
        auto phase_of = [&](std::size_t v) {
            return sc.mode == LoopPlaybackMode::OneShot
                       ? static_cast<double>(v) * 3.0        // stays in-bounds
                       : static_cast<double>(v) * 37.25;
        };

        // Null test: bit-identical output for every voice phase.
        std::size_t mismatches = 0;
        for (std::size_t v = 0; v < kVoices; ++v) {
            auto s_new = make_state(region, phase_of(v));
            auto s_ref = make_state(region, phase_of(v));
            out_new.view().clear();
            out_ref.view().clear();
            SampleVoiceRenderer::render(s_new, out_new.view(), kFrames, scratch,
                                        SampleVoiceRenderOptions{.accumulate = true});
            bench_reference_render(s_ref, out_ref.view(), kFrames, scratch);
            for (std::size_t ch = 0; ch < 2; ++ch)
                for (std::size_t f = 0; f < kFrames; ++f)
                    if (out_new.channel(ch)[f] != out_ref.channel(ch)[f]) ++mismatches;
        }
        REQUIRE(mismatches == 0);  // block kernel is bit-exact vs the scalar path

        std::vector<SampleVoiceRenderState> sn(kVoices), sr(kVoices);
        for (std::size_t v = 0; v < kVoices; ++v) {
            sn[v] = make_state(region, phase_of(v));
            sr[v] = make_state(region, phase_of(v));
        }

        const double us_new = bench_median_us([&] {
            acc.view().clear();
            for (std::size_t v = 0; v < kVoices; ++v) {
                sn[v].position_frames = phase_of(v);
                sn[v].active = true;
                SampleVoiceRenderer::render(sn[v], acc.view(), kFrames, scratch,
                                            SampleVoiceRenderOptions{.accumulate = true});
            }
        }, 200);

        const double us_ref = bench_median_us([&] {
            acc.view().clear();
            for (std::size_t v = 0; v < kVoices; ++v) {
                sr[v].position_frames = phase_of(v);
                bench_reference_render(sr[v], acc.view(), kFrames, scratch);
            }
        }, 200);

        std::printf("  %s : scalar %7.2f us  kernel %7.2f us  speedup %.2fx\n",
                    sc.name, us_ref, us_new, us_ref / us_new);
        REQUIRE(us_new > 0.0);
        REQUIRE(us_ref > 0.0);
    }
    std::printf("\n");
}

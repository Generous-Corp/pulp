#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/sample_voice_renderer.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <cmath>
#include <span>

using Catch::Matchers::WithinAbs;
using pulp::audio::AhdsrEnvelope;
using pulp::audio::AhdsrEnvelopeConfig;
using pulp::audio::Buffer;
using pulp::audio::PublishedSampleStore;
using pulp::audio::PublishedSampleStoreConfig;
using pulp::audio::SamplePool;
using pulp::audio::SamplePoolEntry;
using pulp::audio::SamplePoolResolution;
using pulp::audio::SampleVoiceRenderer;
using pulp::audio::SampleVoiceRenderOptions;
using pulp::audio::SampleVoiceRenderState;

namespace {

void prepare_store(PublishedSampleStore& store, std::span<const float> samples) {
    REQUIRE(store.prepare(PublishedSampleStoreConfig{
        .slot_count = 2,
        .max_channels = 1,
        .max_frames_per_slot = samples.size(),
    }));
    REQUIRE(store.load_mono(samples.data(),
                            static_cast<int>(samples.size()),
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

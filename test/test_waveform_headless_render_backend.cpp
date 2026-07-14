#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/waveform_overview.hpp>
#include <pulp/view/waveform_headless_render_backend.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>

using pulp::audio::AudioFileData;
using pulp::audio::WaveformOverview;
using namespace pulp::view;

namespace {

AudioFileData make_sine(std::uint32_t sample_rate,
                        std::uint64_t num_frames,
                        std::uint32_t num_channels,
                        double frequency_hz,
                        float amplitude = 0.8f) {
    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(num_channels);
    for (auto& channel : data.channels) channel.resize(num_frames);

    const auto phase_inc = 2.0 * std::numbers::pi_v<double> * frequency_hz /
                           static_cast<double>(sample_rate);
    for (std::uint64_t frame = 0; frame < num_frames; ++frame) {
        const auto value = static_cast<float>(std::sin(phase_inc * static_cast<double>(frame))) *
                           amplitude;
        for (auto& channel : data.channels) channel[frame] = value;
    }
    return data;
}

WaveformViewport make_viewport(std::int64_t visible_start = 100,
                               std::int64_t visible_length = 200,
                               float width = 120.0f) {
    WaveformViewport viewport;
    viewport.set_bounds({0.0f, 0.0f, width, 80.0f});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(visible_start, visible_length);
    return viewport;
}

WaveformGpuLayerPlan make_layer_plan(const WaveformOverview& thumbnail,
                                     std::uint64_t source_generation = 1,
                                     int64_t playhead_sample = 150,
                                     bool prefer_gpu = true,
                                     const WaveformGpuUploadKey* previous_key = nullptr) {
    WaveformGpuLayerConfig config;
    config.source_generation = source_generation;
    config.prefer_gpu = prefer_gpu;
    return build_waveform_gpu_layer_plan(
        thumbnail,
        make_viewport(),
        playhead_sample,
        config,
        previous_key);
}

} // namespace

TEST_CASE("Waveform headless backend renders CPU fallback without caching",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 220.0), 10);
    const auto plan = make_layer_plan(thumbnail, 1, 150, false);
    REQUIRE(plan.static_layer.valid);

    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});
    const auto frame = backend.render(thumbnail, plan, {.backend_available = true});

    REQUIRE(frame.status == WaveformHeadlessRenderStatus::CpuFallbackReady);
    REQUIRE(frame.decision.action == WaveformGpuRenderActionKind::CpuFallbackReady);
    REQUIRE(frame.static_vertices.size() == plan.static_layer.vertex_count);
    REQUIRE(frame.resource_id == 0);
    REQUIRE_FALSE(frame.used_cached_resource);
    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.stats().hits == 0);
    REQUIRE(backend.stats().misses == 0);
}

TEST_CASE("Waveform headless backend uploads once and draws cached static vertices",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 330.0), 10);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.valid);

    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});
    const auto uploaded = backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 3});
    REQUIRE(uploaded.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(uploaded.decision.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE(uploaded.resource_id != 0);
    REQUIRE(uploaded.static_vertices.size() == plan.static_layer.vertex_count);
    REQUIRE(backend.resource_count() == 1);
    REQUIRE(backend.stats().misses == 1);

    const auto cached = backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 3});
    REQUIRE(cached.status == WaveformHeadlessRenderStatus::DrawCachedStaticLayer);
    REQUIRE(cached.decision.action == WaveformGpuRenderActionKind::DrawCachedStaticLayer);
    REQUIRE(cached.resource_id == uploaded.resource_id);
    REQUIRE(cached.used_cached_resource);
    REQUIRE(cached.static_vertices.size() == uploaded.static_vertices.size());
    REQUIRE(cached.static_vertices.front().source_peak_index ==
            uploaded.static_vertices.front().source_peak_index);
    REQUIRE(backend.stats().hits == 1);
    REQUIRE(backend.resource_count() == 1);
}

TEST_CASE("Waveform headless backend reuses static payload for playhead-only redraw",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 440.0), 10);
    const auto first_plan = make_layer_plan(thumbnail, 1, 150);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});

    const auto uploaded = backend.render(thumbnail, first_plan, {.backend_available = true, .backend_generation = 5});
    REQUIRE(uploaded.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);

    const auto next_plan = make_layer_plan(
        thumbnail,
        1,
        180,
        true,
        &first_plan.static_layer.upload_key);
    REQUIRE(next_plan.playhead_only_redraw);
    const auto redraw = backend.render(thumbnail, next_plan, {.backend_available = true, .backend_generation = 5});

    REQUIRE(redraw.status == WaveformHeadlessRenderStatus::PlayheadOnlyRedraw);
    REQUIRE(redraw.resource_id == uploaded.resource_id);
    REQUIRE(redraw.static_vertices.size() == uploaded.static_vertices.size());
    REQUIRE(redraw.playhead.visible);
    REQUIRE(redraw.playhead.sample == 180);
    REQUIRE(backend.resource_count() == 1);
}

TEST_CASE("Waveform headless backend releases replaced backend generations",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 550.0), 10);
    const auto plan = make_layer_plan(thumbnail);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 1, .max_staging_vertices = 256});

    const auto first = backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 9});
    REQUIRE(first.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(backend.resource_count() == 1);

    const auto replacement = backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 10});
    REQUIRE(replacement.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(replacement.resource_id != first.resource_id);
    REQUIRE(backend.resource_count() == 1);
    REQUIRE(backend.released_resource_count() == 1);
    REQUIRE(backend.last_released_resource_id() == first.resource_id);

    const auto cached = backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 10});
    REQUIRE(cached.status == WaveformHeadlessRenderStatus::DrawCachedStaticLayer);
    REQUIRE(cached.resource_id == replacement.resource_id);
}

TEST_CASE("Waveform headless backend releases evicted cache payloads on capacity pressure",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 660.0), 10);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 1, .max_staging_vertices = 256});

    const auto first_plan = make_layer_plan(thumbnail, 1);
    const auto first = backend.render(thumbnail, first_plan, {.backend_available = true, .backend_generation = 1});
    REQUIRE(first.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);

    const auto second_plan = make_layer_plan(thumbnail, 2);
    const auto second = backend.render(thumbnail, second_plan, {.backend_available = true, .backend_generation = 1});
    REQUIRE(second.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(second.resource_id != first.resource_id);
    REQUIRE(backend.resource_count() == 1);
    REQUIRE(backend.released_resource_count() == 1);
    REQUIRE(backend.last_released_resource_id() == first.resource_id);
    REQUIRE(backend.stats().evictions == 1);
}

TEST_CASE("Waveform headless backend releases cache payloads when prepared smaller",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 770.0), 10);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});

    const auto first_plan = make_layer_plan(thumbnail, 1);
    const auto second_plan = make_layer_plan(thumbnail, 2);
    const auto first = backend.render(thumbnail, first_plan, {.backend_available = true, .backend_generation = 1});
    const auto second = backend.render(thumbnail, second_plan, {.backend_available = true, .backend_generation = 1});
    REQUIRE(first.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(second.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(backend.resource_count() == 2);

    REQUIRE(backend.prepare({.cache_capacity = 1, .max_staging_vertices = 256}));
    REQUIRE(backend.resource_count() == 1);
    REQUIRE(backend.released_resource_count() == 1);
}

TEST_CASE("Waveform headless backend uses CPU fallback when backend is unavailable",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 880.0), 10);
    const auto plan = make_layer_plan(thumbnail, 1, 150, true);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});

    const auto frame = backend.render(thumbnail, plan, {.backend_available = false, .backend_generation = 0});

    REQUIRE(frame.status == WaveformHeadlessRenderStatus::CpuFallbackReady);
    REQUIRE(frame.decision.action == WaveformGpuRenderActionKind::CpuFallbackReady);
    REQUIRE(frame.static_vertices.size() == plan.static_layer.vertex_count);
    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.stats().misses == 0);
}

TEST_CASE("Waveform headless backend reports staging limits without caching",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 990.0), 10);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.vertex_count > 4);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 4});

    const auto frame = backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 1});

    REQUIRE(frame.status == WaveformHeadlessRenderStatus::NoOp);
    REQUIRE(frame.decision.action == WaveformGpuRenderActionKind::NoOpStagingTooSmall);
    REQUIRE_FALSE(frame.has_static_vertices());
    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.stats().misses == 1);
}

TEST_CASE("Waveform headless backend rejects uploads when cache capacity is zero",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 123.0), 10);
    const auto plan = make_layer_plan(thumbnail);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 0, .max_staging_vertices = 256});

    const auto frame = backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 1});

    REQUIRE(frame.status == WaveformHeadlessRenderStatus::ResourceCommitRejected);
    REQUIRE(frame.decision.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE_FALSE(frame.has_static_vertices());
    REQUIRE(frame.resource_id == 0);
    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.released_resource_count() == 1);
    REQUIRE(backend.stats().entries == 0);
}

TEST_CASE("Waveform headless backend preserves no-fallback backend-unavailable decisions",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 234.0), 10);
    auto plan = make_layer_plan(thumbnail, 1, 150, true);
    plan.static_layer.cpu_fallback_available = false;
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});

    const auto frame = backend.render(thumbnail, plan, {.backend_available = false, .backend_generation = 0});

    REQUIRE(frame.status == WaveformHeadlessRenderStatus::NoOp);
    REQUIRE(frame.decision.action == WaveformGpuRenderActionKind::NoOpBackendUnavailable);
    REQUIRE_FALSE(frame.has_static_vertices());
    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.stats().misses == 0);
}

TEST_CASE("Waveform headless backend clear releases all cached payloads",
          "[view][waveform][headless]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 110.0), 10);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});
    const auto first = backend.render(thumbnail, make_layer_plan(thumbnail, 1), {.backend_available = true, .backend_generation = 1});
    const auto second = backend.render(thumbnail, make_layer_plan(thumbnail, 2), {.backend_available = true, .backend_generation = 1});
    REQUIRE(first.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(second.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(backend.resource_count() == 2);

    backend.clear();

    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.released_resource_count() == 2);
    REQUIRE(backend.stats().entries == 0);
}

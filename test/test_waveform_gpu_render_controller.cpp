#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/waveform_overview.hpp>
#include <pulp/view/waveform_gpu_render_controller.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

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

WaveformViewport make_viewport() {
    WaveformViewport viewport;
    viewport.set_bounds({0.0f, 0.0f, 120.0f, 80.0f});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(100, 200);
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

TEST_CASE("Waveform GPU render controller ignores invalid plans",
          "[view][waveform][gpu]") {
    WaveformGpuRenderController controller(2);
    WaveformOverview empty;
    const auto plan = build_waveform_gpu_layer_plan(empty, make_viewport(), 150);
    std::vector<WaveformPeakVertex> staging(8);

    const auto decision = controller.plan_render(
        empty,
        plan,
        {},
        staging);

    REQUIRE(decision.action == WaveformGpuRenderActionKind::NoOpInvalidPlan);
    REQUIRE(decision.vertices_written == 0);
    REQUIRE(controller.stats().hits == 0);
    REQUIRE(controller.stats().misses == 0);
}

TEST_CASE("Waveform GPU render controller produces CPU fallback without cache stats",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 220.0),
        10);
    WaveformGpuRenderController controller(2);
    const auto plan = make_layer_plan(thumbnail, 1, 150, false);
    REQUIRE(plan.static_layer.valid);

    std::vector<WaveformPeakVertex> staging(plan.static_layer.vertex_count);
    const auto decision = controller.plan_render(
        thumbnail,
        plan,
        {.backend_available = true},
        staging);

    REQUIRE(decision.action == WaveformGpuRenderActionKind::CpuFallbackReady);
    REQUIRE_FALSE(decision.used_gpu_path);
    REQUIRE_FALSE(decision.cache_hit);
    REQUIRE(decision.vertices_written == decision.required_vertices);
    REQUIRE(controller.stats().hits == 0);
    REQUIRE(controller.stats().misses == 0);
}

TEST_CASE("Waveform GPU render controller falls back when backend is unavailable",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 220.0),
        10);
    WaveformGpuRenderController controller(2);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.valid);

    std::vector<WaveformPeakVertex> staging(plan.static_layer.vertex_count);
    const auto decision = controller.plan_render(
        thumbnail,
        plan,
        {.backend_available = false},
        staging);

    REQUIRE(decision.action == WaveformGpuRenderActionKind::CpuFallbackReady);
    REQUIRE_FALSE(decision.used_gpu_path);
    REQUIRE(decision.vertices_written == decision.required_vertices);
    REQUIRE(controller.stats().misses == 0);

    auto no_fallback_plan = plan;
    no_fallback_plan.static_layer.cpu_fallback_available = false;
    const auto unavailable = controller.plan_render(
        thumbnail,
        no_fallback_plan,
        {.backend_available = false},
        staging);
    REQUIRE(unavailable.action == WaveformGpuRenderActionKind::NoOpBackendUnavailable);
    REQUIRE(unavailable.vertices_written == 0);
}

TEST_CASE("Waveform GPU render controller uploads only after backend commit",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 330.0),
        10);
    WaveformGpuRenderController controller(2);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.valid);
    std::vector<WaveformPeakVertex> staging(plan.static_layer.vertex_count);

    const auto first = controller.plan_render(thumbnail, plan, {}, staging);
    REQUIRE(first.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE(first.used_gpu_path);
    REQUIRE_FALSE(first.cache_hit);
    REQUIRE(first.vertices_written == first.required_vertices);
    REQUIRE(controller.size() == 0);
    REQUIRE(controller.stats().misses == 1);

    REQUIRE_FALSE(controller.commit_uploaded_resource(first.upload_key, 101, 0).ok);
    REQUIRE(controller.size() == 0);
    REQUIRE_FALSE(controller.commit_uploaded_resource(WaveformGpuRenderDecision{}, 102, 1).ok);

    const auto commit = controller.commit_uploaded_resource(first, 101, 1);
    REQUIRE(commit.ok);
    REQUIRE_FALSE(commit.evicted);
    REQUIRE(controller.size() == 1);

    const auto cached = controller.plan_render(thumbnail, plan, {}, staging);
    REQUIRE(cached.action == WaveformGpuRenderActionKind::DrawCachedStaticLayer);
    REQUIRE(cached.cache_hit);
    REQUIRE(cached.cached_resource.resource_id == 101);
    REQUIRE(cached.cached_resource.backend_generation == 1);
    REQUIRE(cached.vertices_written == 0);
    REQUIRE(controller.stats().hits == 1);
}

TEST_CASE("Waveform GPU render controller validates playhead-only redraw against cache",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 440.0),
        10);
    WaveformGpuRenderController controller(2);
    const auto first_plan = make_layer_plan(thumbnail, 1, 150);
    REQUIRE(first_plan.static_layer.valid);
    REQUIRE(controller.commit_uploaded_resource(
                first_plan.static_layer.upload_key,
                202,
                first_plan.static_layer.upload_bytes,
                7)
                .ok);

    const auto next_plan = make_layer_plan(
        thumbnail,
        1,
        180,
        true,
        &first_plan.static_layer.upload_key);
    REQUIRE(next_plan.playhead_only_redraw);
    std::vector<WaveformPeakVertex> staging(next_plan.static_layer.vertex_count);

    const auto cached = controller.plan_render(
        thumbnail,
        next_plan,
        {.backend_available = true, .backend_generation = 7},
        staging);
    REQUIRE(cached.action == WaveformGpuRenderActionKind::PlayheadOnlyRedraw);
    REQUIRE(cached.cache_hit);
    REQUIRE(cached.cached_resource.resource_id == 202);
    REQUIRE(cached.cached_resource.backend_generation == 7);

    WaveformGpuResourceRecord removed;
    REQUIRE(controller.erase(first_plan.static_layer.upload_key, removed));
    REQUIRE(removed.resource_id == 202);
    const auto missing = controller.plan_render(
        thumbnail,
        next_plan,
        {.backend_available = true, .backend_generation = 7},
        staging);
    REQUIRE(missing.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE_FALSE(missing.cache_hit);
    REQUIRE(missing.vertices_written == missing.required_vertices);
}

TEST_CASE("Waveform GPU render controller surfaces replacement and eviction records",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 550.0),
        10);
    WaveformGpuRenderController controller(1);
    const auto first_plan = make_layer_plan(thumbnail, 1);
    const auto replacement = controller.commit_uploaded_resource(
        first_plan.static_layer.upload_key,
        301,
        first_plan.static_layer.upload_bytes);
    REQUIRE(replacement.ok);

    const auto replaced = controller.commit_uploaded_resource(
        first_plan.static_layer.upload_key,
        302,
        first_plan.static_layer.upload_bytes + 16);
    REQUIRE(replaced.ok);
    REQUIRE(replaced.replaced);
    REQUIRE(replaced.replaced_record.resource_id == 301);

    const auto second_plan = make_layer_plan(thumbnail, 2);
    const auto evicted = controller.commit_uploaded_resource(
        second_plan.static_layer.upload_key,
        303,
        second_plan.static_layer.upload_bytes);
    REQUIRE(evicted.ok);
    REQUIRE(evicted.evicted);
    REQUIRE(evicted.evicted_record.resource_id == 302);
}

TEST_CASE("Waveform GPU render controller separates backend resource generations",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 605.0),
        10);
    WaveformGpuRenderController controller(1);
    const auto plan = make_layer_plan(thumbnail, 1);
    std::vector<WaveformPeakVertex> staging(plan.static_layer.vertex_count);

    const auto initial_upload = controller.plan_render(
        thumbnail,
        plan,
        {.backend_available = true, .backend_generation = 4},
        staging);
    REQUIRE(initial_upload.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE(initial_upload.backend_generation == 4);
    REQUIRE(controller.commit_uploaded_resource(initial_upload, 501, 4).ok);

    const auto cached = controller.plan_render(
        thumbnail,
        plan,
        {.backend_available = true, .backend_generation = 4},
        staging);
    REQUIRE(cached.action == WaveformGpuRenderActionKind::DrawCachedStaticLayer);
    REQUIRE(cached.cached_resource.resource_id == 501);
    REQUIRE(cached.cached_resource.backend_generation == 4);

    const auto after_reset = controller.plan_render(
        thumbnail,
        plan,
        {.backend_available = true, .backend_generation = 5},
        staging);
    REQUIRE(after_reset.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE_FALSE(after_reset.cache_hit);
    REQUIRE(after_reset.backend_generation == 5);

    REQUIRE_FALSE(controller.commit_uploaded_resource(initial_upload, 503, 5).ok);
    const auto replacement = controller.commit_uploaded_resource(after_reset, 502, 5);
    REQUIRE(replacement.ok);
    REQUIRE(replacement.replaced);
    REQUIRE(replacement.replaced_record.resource_id == 501);
    REQUIRE(replacement.replaced_record.backend_generation == 4);

    const auto refreshed = controller.plan_render(
        thumbnail,
        plan,
        {.backend_available = true, .backend_generation = 5},
        staging);
    REQUIRE(refreshed.action == WaveformGpuRenderActionKind::DrawCachedStaticLayer);
    REQUIRE(refreshed.cached_resource.resource_id == 502);
    REQUIRE(refreshed.cached_resource.backend_generation == 5);

    REQUIRE_FALSE(controller.commit_uploaded_resource(
                              plan.static_layer.upload_key,
                              504,
                              plan.static_layer.upload_bytes,
                              4)
                      .ok);
}

TEST_CASE("Waveform GPU render controller rejects undersized staging without writes",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 660.0),
        10);
    WaveformGpuRenderController controller(2);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.vertex_count > 3);

    std::vector<WaveformPeakVertex> staging(3);
    for (auto& vertex : staging) vertex.source_peak_index = 9999;
    const auto decision = controller.plan_render(thumbnail, plan, {}, staging);

    REQUIRE(decision.action == WaveformGpuRenderActionKind::NoOpStagingTooSmall);
    REQUIRE(decision.vertices_written == 0);
    for (const auto& vertex : staging) REQUIRE(vertex.source_peak_index == 9999);
}

TEST_CASE("Waveform GPU render controller treats source generation as upload identity",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 770.0),
        10);
    WaveformGpuRenderController controller(2);
    const auto old_plan = make_layer_plan(thumbnail, 1);
    REQUIRE(controller.commit_uploaded_resource(
                old_plan.static_layer.upload_key,
                401,
                old_plan.static_layer.upload_bytes)
                .ok);

    const auto new_plan = make_layer_plan(thumbnail, 2);
    REQUIRE(new_plan.static_layer.upload_key.first_peak ==
            old_plan.static_layer.upload_key.first_peak);
    REQUIRE(new_plan.static_layer.upload_key != old_plan.static_layer.upload_key);
    std::vector<WaveformPeakVertex> staging(new_plan.static_layer.vertex_count);

    const auto decision = controller.plan_render(thumbnail, new_plan, {}, staging);
    REQUIRE(decision.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE_FALSE(decision.cache_hit);
    REQUIRE(decision.vertices_written == decision.required_vertices);
}

TEST_CASE("Waveform GPU render controller rejects plan/thumbnail mismatches",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 880.0),
        10);
    const auto mismatched_thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 80, 1, 880.0),
        10);
    WaveformGpuRenderController controller(2);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.valid);
    std::vector<WaveformPeakVertex> staging(plan.static_layer.vertex_count);

    const auto decision = controller.plan_render(mismatched_thumbnail, plan, {}, staging);
    REQUIRE(decision.action == WaveformGpuRenderActionKind::NoOpVertexFillFailed);
    REQUIRE(decision.vertices_written < decision.required_vertices);
}

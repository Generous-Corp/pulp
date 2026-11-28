#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/waveform_overview.hpp>
#include <pulp/vellum_audio/waveform_gpu_primitives.hpp>
#include <pulp/vellum_audio/waveform_gpu_render_controller.hpp>
#include <pulp/vellum_audio/waveform_headless_render_backend.hpp>

#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

using pulp::audio::AudioFileData;
using pulp::audio::WaveformOverview;
using namespace pulp::vellum_audio;

namespace {

AudioFileData make_sine(std::uint32_t sample_rate, std::uint64_t num_frames,
                        std::uint32_t num_channels, double frequency_hz, float amplitude = 0.8f) {
    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(num_channels);
    for (auto& channel : data.channels)
        channel.resize(num_frames);

    const auto phase_inc =
        2.0 * std::numbers::pi_v<double> * frequency_hz / static_cast<double>(sample_rate);
    for (std::uint64_t frame = 0; frame < num_frames; ++frame) {
        const auto value =
            static_cast<float>(std::sin(phase_inc * static_cast<double>(frame))) * amplitude;
        for (auto& channel : data.channels) {
            channel[frame] = value;
        }
    }
    return data;
}

WaveformViewport make_viewport(float width = 120.0f, float height = 80.0f) {
    WaveformViewport viewport;
    viewport.set_bounds({0.0f, 0.0f, width, height});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(100, 200);
    return viewport;
}

WaveformGpuUploadKey shifted_key(WaveformGpuUploadKey key, std::uint32_t first_peak) {
    key.first_peak = first_peak;
    return key;
}

WaveformGpuLayerConfig make_config(std::uint64_t source_generation = 1) {
    WaveformGpuLayerConfig config;
    config.source_generation = source_generation;
    return config;
}

} // namespace

TEST_CASE("Waveform GPU static layer plan keys thumbnail LOD ranges", "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 2, 440.0), 10);
    WaveformGpuLayerConfig config;
    config.source_generation = 7;
    config.channel = WaveformOverview::kAllChannels;

    const auto plan = build_waveform_gpu_static_layer_plan(thumbnail, make_viewport(), config);

    REQUIRE(plan.valid);
    REQUIRE(plan.prefer_gpu);
    REQUIRE(plan.cpu_fallback_available);
    REQUIRE(plan.upload_key.valid());
    REQUIRE(plan.upload_key.source_generation == 7);
    REQUIRE(plan.upload_key.channel == WaveformOverview::kAllChannels);
    REQUIRE(plan.upload_key.samples_per_peak == 10);
    REQUIRE(plan.upload_key.first_peak == 10);
    REQUIRE(plan.upload_key.peak_count == 20);
    REQUIRE(plan.upload_key.visible_start == 100);
    REQUIRE(plan.upload_key.visible_length == 200);
    REQUIRE(plan.upload_key.bounds_x_milli_px == 0);
    REQUIRE(plan.upload_key.bounds_y_milli_px == 0);
    REQUIRE(plan.upload_key.bounds_width_milli_px == 120000);
    REQUIRE(plan.upload_key.bounds_height_milli_px == 80000);
    REQUIRE(plan.upload_key.target_width_px == 120);
    REQUIRE(plan.upload_key.target_height_px == 80);
    REQUIRE(plan.vertex_count == 20);
    REQUIRE(plan.upload_bytes == plan.vertex_count * sizeof(WaveformPeakVertex));
}

TEST_CASE("Waveform GPU upload keys include baked viewport geometry", "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 220.0), 10);
    auto base_viewport = make_viewport();
    base_viewport.set_visible_range(100, 178);
    const auto config = make_config();
    const auto base = build_waveform_gpu_static_layer_plan(thumbnail, base_viewport, config);
    REQUIRE(base.valid);

    auto panned = base_viewport;
    panned.set_visible_range(101, 178);
    const auto panned_plan = build_waveform_gpu_static_layer_plan(thumbnail, panned, config);
    REQUIRE(panned_plan.valid);
    REQUIRE(panned_plan.upload_key.first_peak == base.upload_key.first_peak);
    REQUIRE(panned_plan.upload_key.peak_count == base.upload_key.peak_count);
    REQUIRE(panned_plan.upload_key != base.upload_key);

    auto moved = base_viewport;
    moved.set_bounds({5.0f, 7.0f, 120.0f, 80.0f});
    const auto moved_plan = build_waveform_gpu_static_layer_plan(thumbnail, moved, config);
    REQUIRE(moved_plan.valid);
    REQUIRE(moved_plan.upload_key.first_peak == base.upload_key.first_peak);
    REQUIRE(moved_plan.upload_key.peak_count == base.upload_key.peak_count);
    REQUIRE(moved_plan.upload_key != base.upload_key);
}

TEST_CASE("Waveform GPU layer plan supports playhead-only redraw", "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 220.0), 10);
    WaveformGpuLayerConfig config;
    config.source_generation = 3;
    const auto viewport = make_viewport();
    const auto first = build_waveform_gpu_layer_plan(thumbnail, viewport, 150, config);
    REQUIRE(first.static_layer.valid);
    REQUIRE(first.static_layer_dirty);
    REQUIRE_FALSE(first.playhead_only_redraw);
    REQUIRE(first.playhead.visible);

    const auto second = build_waveform_gpu_layer_plan(thumbnail, viewport, 180, config,
                                                      &first.static_layer.upload_key);
    REQUIRE_FALSE(second.static_layer_dirty);
    REQUIRE(second.playhead_only_redraw);
    REQUIRE(second.playhead.visible);
    REQUIRE(second.playhead.x == Catch::Approx(48.0f));

    auto zoomed = viewport;
    zoomed.set_visible_range(100, 100);
    const auto changed = build_waveform_gpu_layer_plan(thumbnail, zoomed, 180, config,
                                                       &first.static_layer.upload_key);
    REQUIRE(changed.static_layer_dirty);
    REQUIRE_FALSE(changed.playhead_only_redraw);
}

TEST_CASE("Waveform GPU vertex fill provides CPU fallback upload data", "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 2, 110.0), 10);
    const auto plan =
        build_waveform_gpu_static_layer_plan(thumbnail, make_viewport(), make_config());
    REQUIRE(plan.valid);

    std::vector<WaveformPeakVertex> vertices(plan.vertex_count);
    const auto written = fill_waveform_peak_vertices(thumbnail, plan, vertices);
    REQUIRE(written == plan.vertex_count);

    for (const auto& vertex : vertices) {
        REQUIRE(vertex.x >= 0.0f);
        REQUIRE(vertex.x <= 120.0f);
        REQUIRE(vertex.y_min >= 0.0f);
        REQUIRE(vertex.y_max <= 80.0f);
        REQUIRE(vertex.y_min <= vertex.y_max);
        REQUIRE(vertex.min_value >= -1.0f);
        REQUIRE(vertex.max_value <= 1.0f);
        REQUIRE(vertex.min_value <= vertex.max_value);
        // Screen y grows downward from the 80 px viewport's centre line at 40.
        REQUIRE(vertex.y_min == Catch::Approx(40.0f - vertex.max_value * 40.0f).margin(1e-3));
        REQUIRE(vertex.y_max == Catch::Approx(40.0f - vertex.min_value * 40.0f).margin(1e-3));
    }
    REQUIRE(vertices.front().source_peak_index == plan.upload_key.first_peak);

    std::vector<WaveformPeakVertex> partial(3);
    REQUIRE(fill_waveform_peak_vertices(thumbnail, plan, partial) == 3);
    REQUIRE(partial[2].source_peak_index == plan.upload_key.first_peak + 2);
}

TEST_CASE("Waveform GPU primitive rejects invalid source or viewport state",
          "[view][waveform][gpu]") {
    WaveformOverview empty;
    const auto invalid_thumbnail = build_waveform_gpu_static_layer_plan(empty, make_viewport());
    REQUIRE_FALSE(invalid_thumbnail.valid);

    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 330.0), 10);
    auto empty_view = make_viewport();
    empty_view.set_bounds({0.0f, 0.0f, 0.0f, 80.0f});
    const auto invalid_view =
        build_waveform_gpu_static_layer_plan(thumbnail, empty_view, make_config());
    REQUIRE_FALSE(invalid_view.valid);

    WaveformGpuLayerConfig invalid_channel;
    invalid_channel.source_generation = 1;
    invalid_channel.channel = 4;
    const auto invalid_channel_plan =
        build_waveform_gpu_static_layer_plan(thumbnail, make_viewport(), invalid_channel);
    REQUIRE_FALSE(invalid_channel_plan.valid);

    std::vector<WaveformPeakVertex> vertices(4);
    REQUIRE(fill_waveform_peak_vertices(thumbnail, invalid_view, vertices) == 0);
}

TEST_CASE("Waveform GPU resource cache tracks opaque backend handles with LRU eviction",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 440.0), 10);
    const auto base_plan =
        build_waveform_gpu_static_layer_plan(thumbnail, make_viewport(), make_config());
    REQUIRE(base_plan.valid);

    auto key_a = base_plan.upload_key;
    auto key_b = shifted_key(key_a, key_a.first_peak + 10);
    auto key_c = shifted_key(key_a, key_a.first_peak + 20);

    WaveformGpuResourceCache cache(2);
    REQUIRE(cache.put(key_a, 101, base_plan.upload_bytes).ok);
    REQUIRE(cache.put(key_b, 202, 64).ok);
    REQUIRE(cache.size() == 2);

    const auto* a = cache.find(key_a);
    REQUIRE(a != nullptr);
    REQUIRE(a->resource_id == 101);
    REQUIRE(cache.find(key_c) == nullptr);

    const auto put_c = cache.put(key_c, 303, 128);
    REQUIRE(put_c.ok);
    REQUIRE(put_c.evicted);
    REQUIRE(put_c.evicted_record.resource_id == 202);
    REQUIRE(cache.size() == 2);
    REQUIRE(cache.find(key_a) != nullptr);
    REQUIRE(cache.find(key_b) == nullptr);
    REQUIRE(cache.find(key_c) != nullptr);

    const auto stats = cache.stats();
    REQUIRE(stats.entries == 2);
    REQUIRE(stats.capacity == 2);
    REQUIRE(stats.hits == 3);
    REQUIRE(stats.misses == 2);
    REQUIRE(stats.evictions == 1);
    REQUIRE(stats.bytes == base_plan.upload_bytes + 128);
}

TEST_CASE("Waveform GPU resource cache rejects invalid records and can shrink",
          "[view][waveform][gpu]") {
    WaveformGpuResourceCache cache(2);
    WaveformGpuUploadKey invalid;
    REQUIRE_FALSE(cache.put(invalid, 1, 16).ok);

    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 440.0), 10);
    const auto plan =
        build_waveform_gpu_static_layer_plan(thumbnail, make_viewport(), make_config());
    REQUIRE(plan.valid);
    REQUIRE_FALSE(cache.put(plan.upload_key, 0, 16).ok);
    REQUIRE_FALSE(cache.put(plan.upload_key, 10, 0).ok);
    REQUIRE(cache.put(plan.upload_key, 10, 16).ok);
    const auto replaced = cache.put(plan.upload_key, 11, 24);
    REQUIRE(replaced.ok);
    REQUIRE(replaced.replaced);
    REQUIRE(replaced.replaced_record.resource_id == 10);
    REQUIRE(replaced.replaced_record.bytes == 16);
    REQUIRE(cache.size() == 1);

    WaveformGpuResourceRecord removed;
    REQUIRE(cache.erase(plan.upload_key, removed));
    REQUIRE(removed.resource_id == 11);
    REQUIRE(cache.size() == 0);

    REQUIRE(cache.put(plan.upload_key, 12, 32).ok);
    std::vector<WaveformGpuResourceRecord> evicted;
    REQUIRE_FALSE(cache.prepare(0));
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.prepare(0, &evicted));
    REQUIRE(evicted.size() == 1);
    REQUIRE(evicted[0].resource_id == 12);
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.capacity() == 0);
    REQUIRE_FALSE(cache.put(plan.upload_key, 13, 16).ok);
}

TEST_CASE("Waveform GPU resource cache shrinking evicts the least recently used records first",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 440.0), 10);
    const auto plan =
        build_waveform_gpu_static_layer_plan(thumbnail, make_viewport(), make_config());
    REQUIRE(plan.valid);
    const auto key_a = plan.upload_key;
    const auto key_b = shifted_key(key_a, key_a.first_peak + 10);
    const auto key_c = shifted_key(key_a, key_a.first_peak + 20);

    WaveformGpuResourceCache cache(3);
    REQUIRE(cache.put(key_a, 1, 16).ok);
    REQUIRE(cache.put(key_b, 2, 16).ok);
    REQUIRE(cache.put(key_c, 3, 16).ok);
    // Touch a, then c: b is now the least recently used, then a.
    REQUIRE(cache.find(key_a) != nullptr);
    REQUIRE(cache.find(key_c) != nullptr);

    std::vector<WaveformGpuResourceRecord> evicted;
    REQUIRE(cache.prepare(1, &evicted));
    REQUIRE(evicted.size() == 2);
    REQUIRE(evicted[0].resource_id == 2);
    REQUIRE(evicted[1].resource_id == 1);
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.find(key_c) != nullptr);
}

TEST_CASE("Waveform GPU upload plans require explicit source generation", "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 440.0), 10);

    const auto implicit = build_waveform_gpu_static_layer_plan(thumbnail, make_viewport());
    REQUIRE_FALSE(implicit.valid);
    REQUIRE_FALSE(implicit.upload_key.valid());

    const auto explicit_plan =
        build_waveform_gpu_static_layer_plan(thumbnail, make_viewport(), make_config(9));
    REQUIRE(explicit_plan.valid);
    REQUIRE(explicit_plan.upload_key.valid());
    REQUIRE(explicit_plan.upload_key.source_generation == 9);
}

TEST_CASE("Waveform GPU resource cache returns records for release-safe clearing",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 440.0), 10);
    const auto plan =
        build_waveform_gpu_static_layer_plan(thumbnail, make_viewport(), make_config());
    REQUIRE(plan.valid);

    WaveformGpuResourceCache cache(2);
    REQUIRE(cache.put(plan.upload_key, 20, 64).ok);
    auto second_key = shifted_key(plan.upload_key, plan.upload_key.first_peak + 1);
    REQUIRE(cache.put(second_key, 21, 64).ok);

    auto removed = cache.clear_and_return_records();
    REQUIRE(removed.size() == 2);
    REQUIRE(removed[0].resource_id == 20);
    REQUIRE(removed[1].resource_id == 21);
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.capacity() == 2);
}

// ---- WaveformGpuRenderController ----
namespace render_controller_tests {
namespace {

AudioFileData make_sine(std::uint32_t sample_rate, std::uint64_t num_frames,
                        std::uint32_t num_channels, double frequency_hz, float amplitude = 0.8f) {
    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(num_channels);
    for (auto& channel : data.channels)
        channel.resize(num_frames);

    const auto phase_inc =
        2.0 * std::numbers::pi_v<double> * frequency_hz / static_cast<double>(sample_rate);
    for (std::uint64_t frame = 0; frame < num_frames; ++frame) {
        const auto value =
            static_cast<float>(std::sin(phase_inc * static_cast<double>(frame))) * amplitude;
        for (auto& channel : data.channels)
            channel[frame] = value;
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
                                     int64_t playhead_sample = 150, bool prefer_gpu = true,
                                     const WaveformGpuUploadKey* previous_key = nullptr) {
    WaveformGpuLayerConfig config;
    config.source_generation = source_generation;
    config.prefer_gpu = prefer_gpu;
    return build_waveform_gpu_layer_plan(thumbnail, make_viewport(), playhead_sample, config,
                                         previous_key);
}

} // namespace

TEST_CASE("Waveform GPU render controller ignores invalid plans", "[view][waveform][gpu]") {
    WaveformGpuRenderController controller(2);
    WaveformOverview empty;
    const auto plan = build_waveform_gpu_layer_plan(empty, make_viewport(), 150);
    std::vector<WaveformPeakVertex> staging(8);

    const auto decision = controller.plan_render(empty, plan, {}, staging);

    REQUIRE(decision.action == WaveformGpuRenderActionKind::NoOpInvalidPlan);
    REQUIRE(decision.vertices_written == 0);
    REQUIRE(controller.stats().hits == 0);
    REQUIRE(controller.stats().misses == 0);
}

TEST_CASE("Waveform GPU render controller produces CPU fallback without cache stats",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 220.0), 10);
    WaveformGpuRenderController controller(2);
    const auto plan = make_layer_plan(thumbnail, 1, 150, false);
    REQUIRE(plan.static_layer.valid);

    std::vector<WaveformPeakVertex> staging(plan.static_layer.vertex_count);
    const auto decision =
        controller.plan_render(thumbnail, plan, {.backend_available = true}, staging);

    REQUIRE(decision.action == WaveformGpuRenderActionKind::CpuFallbackReady);
    REQUIRE_FALSE(decision.used_gpu_path);
    REQUIRE_FALSE(decision.cache_hit);
    REQUIRE(decision.vertices_written == decision.required_vertices);
    REQUIRE(controller.stats().hits == 0);
    REQUIRE(controller.stats().misses == 0);
}

TEST_CASE("Waveform GPU render controller falls back when backend is unavailable",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 220.0), 10);
    WaveformGpuRenderController controller(2);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.valid);

    std::vector<WaveformPeakVertex> staging(plan.static_layer.vertex_count);
    const auto decision =
        controller.plan_render(thumbnail, plan, {.backend_available = false}, staging);

    REQUIRE(decision.action == WaveformGpuRenderActionKind::CpuFallbackReady);
    REQUIRE_FALSE(decision.used_gpu_path);
    REQUIRE(decision.vertices_written == decision.required_vertices);
    REQUIRE(controller.stats().misses == 0);

    auto no_fallback_plan = plan;
    no_fallback_plan.static_layer.cpu_fallback_available = false;
    const auto unavailable =
        controller.plan_render(thumbnail, no_fallback_plan, {.backend_available = false}, staging);
    REQUIRE(unavailable.action == WaveformGpuRenderActionKind::NoOpBackendUnavailable);
    REQUIRE(unavailable.vertices_written == 0);
}

TEST_CASE("Waveform GPU render controller uploads only after backend commit",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 330.0), 10);
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
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 440.0), 10);
    WaveformGpuRenderController controller(2);
    const auto first_plan = make_layer_plan(thumbnail, 1, 150);
    REQUIRE(first_plan.static_layer.valid);
    REQUIRE(controller
                .commit_uploaded_resource(first_plan.static_layer.upload_key, 202,
                                          first_plan.static_layer.upload_bytes, 7)
                .ok);

    const auto next_plan =
        make_layer_plan(thumbnail, 1, 180, true, &first_plan.static_layer.upload_key);
    REQUIRE(next_plan.playhead_only_redraw);
    std::vector<WaveformPeakVertex> staging(next_plan.static_layer.vertex_count);

    const auto cached = controller.plan_render(
        thumbnail, next_plan, {.backend_available = true, .backend_generation = 7}, staging);
    REQUIRE(cached.action == WaveformGpuRenderActionKind::PlayheadOnlyRedraw);
    REQUIRE(cached.cache_hit);
    REQUIRE(cached.cached_resource.resource_id == 202);
    REQUIRE(cached.cached_resource.backend_generation == 7);

    WaveformGpuResourceRecord removed;
    REQUIRE(controller.erase(first_plan.static_layer.upload_key, removed));
    REQUIRE(removed.resource_id == 202);
    const auto missing = controller.plan_render(
        thumbnail, next_plan, {.backend_available = true, .backend_generation = 7}, staging);
    REQUIRE(missing.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE_FALSE(missing.cache_hit);
    REQUIRE(missing.vertices_written == missing.required_vertices);
}

TEST_CASE("Waveform GPU render controller surfaces replacement and eviction records",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 550.0), 10);
    WaveformGpuRenderController controller(1);
    const auto first_plan = make_layer_plan(thumbnail, 1);
    const auto replacement = controller.commit_uploaded_resource(
        first_plan.static_layer.upload_key, 301, first_plan.static_layer.upload_bytes);
    REQUIRE(replacement.ok);

    const auto replaced = controller.commit_uploaded_resource(
        first_plan.static_layer.upload_key, 302, first_plan.static_layer.upload_bytes + 16);
    REQUIRE(replaced.ok);
    REQUIRE(replaced.replaced);
    REQUIRE(replaced.replaced_record.resource_id == 301);

    const auto second_plan = make_layer_plan(thumbnail, 2);
    const auto evicted = controller.commit_uploaded_resource(
        second_plan.static_layer.upload_key, 303, second_plan.static_layer.upload_bytes);
    REQUIRE(evicted.ok);
    REQUIRE(evicted.evicted);
    REQUIRE(evicted.evicted_record.resource_id == 302);
}

TEST_CASE("Waveform GPU render controller separates backend resource generations",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 605.0), 10);
    WaveformGpuRenderController controller(1);
    const auto plan = make_layer_plan(thumbnail, 1);
    std::vector<WaveformPeakVertex> staging(plan.static_layer.vertex_count);

    const auto initial_upload = controller.plan_render(
        thumbnail, plan, {.backend_available = true, .backend_generation = 4}, staging);
    REQUIRE(initial_upload.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE(initial_upload.backend_generation == 4);
    REQUIRE(controller.commit_uploaded_resource(initial_upload, 501, 4).ok);

    const auto cached = controller.plan_render(
        thumbnail, plan, {.backend_available = true, .backend_generation = 4}, staging);
    REQUIRE(cached.action == WaveformGpuRenderActionKind::DrawCachedStaticLayer);
    REQUIRE(cached.cached_resource.resource_id == 501);
    REQUIRE(cached.cached_resource.backend_generation == 4);

    const auto after_reset = controller.plan_render(
        thumbnail, plan, {.backend_available = true, .backend_generation = 5}, staging);
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
        thumbnail, plan, {.backend_available = true, .backend_generation = 5}, staging);
    REQUIRE(refreshed.action == WaveformGpuRenderActionKind::DrawCachedStaticLayer);
    REQUIRE(refreshed.cached_resource.resource_id == 502);
    REQUIRE(refreshed.cached_resource.backend_generation == 5);

    REQUIRE_FALSE(controller
                      .commit_uploaded_resource(plan.static_layer.upload_key, 504,
                                                plan.static_layer.upload_bytes, 4)
                      .ok);
}

TEST_CASE("Waveform GPU render controller rejects undersized staging without writes",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 660.0), 10);
    WaveformGpuRenderController controller(2);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.vertex_count > 3);

    std::vector<WaveformPeakVertex> staging(3);
    for (auto& vertex : staging)
        vertex.source_peak_index = 9999;
    const auto decision = controller.plan_render(thumbnail, plan, {}, staging);

    REQUIRE(decision.action == WaveformGpuRenderActionKind::NoOpStagingTooSmall);
    REQUIRE(decision.vertices_written == 0);
    for (const auto& vertex : staging)
        REQUIRE(vertex.source_peak_index == 9999);
}

TEST_CASE("Waveform GPU render controller treats source generation as upload identity",
          "[view][waveform][gpu]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 770.0), 10);
    WaveformGpuRenderController controller(2);
    const auto old_plan = make_layer_plan(thumbnail, 1);
    REQUIRE(controller
                .commit_uploaded_resource(old_plan.static_layer.upload_key, 401,
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
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 880.0), 10);
    const auto mismatched_thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 80, 1, 880.0), 10);
    WaveformGpuRenderController controller(2);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.valid);
    std::vector<WaveformPeakVertex> staging(plan.static_layer.vertex_count);

    const auto decision = controller.plan_render(mismatched_thumbnail, plan, {}, staging);
    REQUIRE(decision.action == WaveformGpuRenderActionKind::NoOpVertexFillFailed);
    REQUIRE(decision.vertices_written < decision.required_vertices);
}
} // namespace render_controller_tests

// ---- WaveformHeadlessRenderBackend ----
namespace headless_backend_tests {
namespace {

AudioFileData make_sine(std::uint32_t sample_rate, std::uint64_t num_frames,
                        std::uint32_t num_channels, double frequency_hz, float amplitude = 0.8f) {
    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(num_channels);
    for (auto& channel : data.channels)
        channel.resize(num_frames);

    const auto phase_inc =
        2.0 * std::numbers::pi_v<double> * frequency_hz / static_cast<double>(sample_rate);
    for (std::uint64_t frame = 0; frame < num_frames; ++frame) {
        const auto value =
            static_cast<float>(std::sin(phase_inc * static_cast<double>(frame))) * amplitude;
        for (auto& channel : data.channels)
            channel[frame] = value;
    }
    return data;
}

WaveformViewport make_viewport(std::int64_t visible_start = 100, std::int64_t visible_length = 200,
                               float width = 120.0f) {
    WaveformViewport viewport;
    viewport.set_bounds({0.0f, 0.0f, width, 80.0f});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(visible_start, visible_length);
    return viewport;
}

WaveformGpuLayerPlan make_layer_plan(const WaveformOverview& thumbnail,
                                     std::uint64_t source_generation = 1,
                                     int64_t playhead_sample = 150, bool prefer_gpu = true,
                                     const WaveformGpuUploadKey* previous_key = nullptr) {
    WaveformGpuLayerConfig config;
    config.source_generation = source_generation;
    config.prefer_gpu = prefer_gpu;
    return build_waveform_gpu_layer_plan(thumbnail, make_viewport(), playhead_sample, config,
                                         previous_key);
}

} // namespace

TEST_CASE("Waveform headless backend renders CPU fallback without caching",
          "[view][waveform][headless]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 220.0), 10);
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
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 330.0), 10);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.valid);

    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});
    const auto uploaded =
        backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 3});
    REQUIRE(uploaded.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(uploaded.decision.action == WaveformGpuRenderActionKind::UploadStaticLayerReady);
    REQUIRE(uploaded.resource_id != 0);
    REQUIRE(uploaded.static_vertices.size() == plan.static_layer.vertex_count);
    REQUIRE(backend.resource_count() == 1);
    REQUIRE(backend.stats().misses == 1);

    const auto cached =
        backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 3});
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
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 440.0), 10);
    const auto first_plan = make_layer_plan(thumbnail, 1, 150);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});

    const auto uploaded =
        backend.render(thumbnail, first_plan, {.backend_available = true, .backend_generation = 5});
    REQUIRE(uploaded.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);

    const auto next_plan =
        make_layer_plan(thumbnail, 1, 180, true, &first_plan.static_layer.upload_key);
    REQUIRE(next_plan.playhead_only_redraw);
    const auto redraw =
        backend.render(thumbnail, next_plan, {.backend_available = true, .backend_generation = 5});

    REQUIRE(redraw.status == WaveformHeadlessRenderStatus::PlayheadOnlyRedraw);
    REQUIRE(redraw.resource_id == uploaded.resource_id);
    REQUIRE(redraw.static_vertices.size() == uploaded.static_vertices.size());
    REQUIRE(redraw.playhead.visible);
    REQUIRE(redraw.playhead.sample == 180);
    REQUIRE(backend.resource_count() == 1);
}

TEST_CASE("Waveform headless backend releases replaced backend generations",
          "[view][waveform][headless]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 550.0), 10);
    const auto plan = make_layer_plan(thumbnail);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 1, .max_staging_vertices = 256});

    const auto first =
        backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 9});
    REQUIRE(first.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(backend.resource_count() == 1);

    const auto replacement =
        backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 10});
    REQUIRE(replacement.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(replacement.resource_id != first.resource_id);
    REQUIRE(backend.resource_count() == 1);
    REQUIRE(backend.released_resource_count() == 1);
    REQUIRE(backend.last_released_resource_id() == first.resource_id);

    const auto cached =
        backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 10});
    REQUIRE(cached.status == WaveformHeadlessRenderStatus::DrawCachedStaticLayer);
    REQUIRE(cached.resource_id == replacement.resource_id);
}

TEST_CASE("Waveform headless backend releases evicted cache payloads on capacity pressure",
          "[view][waveform][headless]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 660.0), 10);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 1, .max_staging_vertices = 256});

    const auto first_plan = make_layer_plan(thumbnail, 1);
    const auto first =
        backend.render(thumbnail, first_plan, {.backend_available = true, .backend_generation = 1});
    REQUIRE(first.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);

    const auto second_plan = make_layer_plan(thumbnail, 2);
    const auto second = backend.render(thumbnail, second_plan,
                                       {.backend_available = true, .backend_generation = 1});
    REQUIRE(second.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(second.resource_id != first.resource_id);
    REQUIRE(backend.resource_count() == 1);
    REQUIRE(backend.released_resource_count() == 1);
    REQUIRE(backend.last_released_resource_id() == first.resource_id);
    REQUIRE(backend.stats().evictions == 1);
}

TEST_CASE("Waveform headless backend releases cache payloads when prepared smaller",
          "[view][waveform][headless]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 770.0), 10);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});

    const auto first_plan = make_layer_plan(thumbnail, 1);
    const auto second_plan = make_layer_plan(thumbnail, 2);
    const auto first =
        backend.render(thumbnail, first_plan, {.backend_available = true, .backend_generation = 1});
    const auto second = backend.render(thumbnail, second_plan,
                                       {.backend_available = true, .backend_generation = 1});
    REQUIRE(first.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(second.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(backend.resource_count() == 2);

    REQUIRE(backend.prepare({.cache_capacity = 1, .max_staging_vertices = 256}));
    REQUIRE(backend.resource_count() == 1);
    REQUIRE(backend.released_resource_count() == 1);
}

TEST_CASE("Waveform headless backend uses CPU fallback when backend is unavailable",
          "[view][waveform][headless]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 880.0), 10);
    const auto plan = make_layer_plan(thumbnail, 1, 150, true);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});

    const auto frame =
        backend.render(thumbnail, plan, {.backend_available = false, .backend_generation = 0});

    REQUIRE(frame.status == WaveformHeadlessRenderStatus::CpuFallbackReady);
    REQUIRE(frame.decision.action == WaveformGpuRenderActionKind::CpuFallbackReady);
    REQUIRE(frame.static_vertices.size() == plan.static_layer.vertex_count);
    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.stats().misses == 0);
}

TEST_CASE("Waveform headless backend reports staging limits without caching",
          "[view][waveform][headless]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 990.0), 10);
    const auto plan = make_layer_plan(thumbnail);
    REQUIRE(plan.static_layer.vertex_count > 4);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 4});

    const auto frame =
        backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 1});

    REQUIRE(frame.status == WaveformHeadlessRenderStatus::NoOp);
    REQUIRE(frame.decision.action == WaveformGpuRenderActionKind::NoOpStagingTooSmall);
    REQUIRE_FALSE(frame.has_static_vertices());
    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.stats().misses == 1);
}

TEST_CASE("Waveform headless backend rejects uploads when cache capacity is zero",
          "[view][waveform][headless]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 123.0), 10);
    const auto plan = make_layer_plan(thumbnail);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 0, .max_staging_vertices = 256});

    const auto frame =
        backend.render(thumbnail, plan, {.backend_available = true, .backend_generation = 1});

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
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 234.0), 10);
    auto plan = make_layer_plan(thumbnail, 1, 150, true);
    plan.static_layer.cpu_fallback_available = false;
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});

    const auto frame =
        backend.render(thumbnail, plan, {.backend_available = false, .backend_generation = 0});

    REQUIRE(frame.status == WaveformHeadlessRenderStatus::NoOp);
    REQUIRE(frame.decision.action == WaveformGpuRenderActionKind::NoOpBackendUnavailable);
    REQUIRE_FALSE(frame.has_static_vertices());
    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.stats().misses == 0);
}

TEST_CASE("Waveform headless backend clear releases all cached payloads",
          "[view][waveform][headless]") {
    const auto thumbnail =
        WaveformOverview::build_from_buffer(make_sine(48000, 1000, 1, 110.0), 10);
    WaveformHeadlessRenderBackend backend({.cache_capacity = 2, .max_staging_vertices = 256});
    const auto first = backend.render(thumbnail, make_layer_plan(thumbnail, 1),
                                      {.backend_available = true, .backend_generation = 1});
    const auto second = backend.render(thumbnail, make_layer_plan(thumbnail, 2),
                                       {.backend_available = true, .backend_generation = 1});
    REQUIRE(first.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(second.status == WaveformHeadlessRenderStatus::UploadedStaticLayer);
    REQUIRE(backend.resource_count() == 2);

    backend.clear();

    REQUIRE(backend.resource_count() == 0);
    REQUIRE(backend.released_resource_count() == 2);
    REQUIRE(backend.stats().entries == 0);
}
} // namespace headless_backend_tests

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/waveform_overview.hpp>
#include <pulp/view/waveform_gpu_primitives.hpp>

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
        const auto value = static_cast<float>(std::sin(phase_inc * static_cast<double>(frame))) * amplitude;
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

TEST_CASE("Waveform GPU static layer plan keys thumbnail LOD ranges",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 2, 440.0),
        10);
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

TEST_CASE("Waveform GPU upload keys include baked viewport geometry",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 220.0),
        10);
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

TEST_CASE("Waveform GPU layer plan supports playhead-only redraw",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 220.0),
        10);
    WaveformGpuLayerConfig config;
    config.source_generation = 3;
    const auto viewport = make_viewport();
    const auto first = build_waveform_gpu_layer_plan(thumbnail, viewport, 150, config);
    REQUIRE(first.static_layer.valid);
    REQUIRE(first.static_layer_dirty);
    REQUIRE_FALSE(first.playhead_only_redraw);
    REQUIRE(first.playhead.visible);

    const auto second = build_waveform_gpu_layer_plan(
        thumbnail,
        viewport,
        180,
        config,
        &first.static_layer.upload_key);
    REQUIRE_FALSE(second.static_layer_dirty);
    REQUIRE(second.playhead_only_redraw);
    REQUIRE(second.playhead.visible);
    REQUIRE(second.playhead.x == Catch::Approx(48.0f));

    auto zoomed = viewport;
    zoomed.set_visible_range(100, 100);
    const auto changed = build_waveform_gpu_layer_plan(
        thumbnail,
        zoomed,
        180,
        config,
        &first.static_layer.upload_key);
    REQUIRE(changed.static_layer_dirty);
    REQUIRE_FALSE(changed.playhead_only_redraw);
}

TEST_CASE("Waveform GPU vertex fill provides CPU fallback upload data",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 2, 110.0),
        10);
    const auto plan = build_waveform_gpu_static_layer_plan(
        thumbnail,
        make_viewport(),
        make_config());
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

    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 330.0),
        10);
    auto empty_view = make_viewport();
    empty_view.set_bounds({0.0f, 0.0f, 0.0f, 80.0f});
    const auto invalid_view = build_waveform_gpu_static_layer_plan(
        thumbnail,
        empty_view,
        make_config());
    REQUIRE_FALSE(invalid_view.valid);

    WaveformGpuLayerConfig invalid_channel;
    invalid_channel.source_generation = 1;
    invalid_channel.channel = 4;
    const auto invalid_channel_plan = build_waveform_gpu_static_layer_plan(
        thumbnail,
        make_viewport(),
        invalid_channel);
    REQUIRE_FALSE(invalid_channel_plan.valid);

    std::vector<WaveformPeakVertex> vertices(4);
    REQUIRE(fill_waveform_peak_vertices(thumbnail, invalid_view, vertices) == 0);
}

TEST_CASE("Waveform GPU resource cache tracks opaque backend handles with LRU eviction",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 440.0),
        10);
    const auto base_plan = build_waveform_gpu_static_layer_plan(
        thumbnail,
        make_viewport(),
        make_config());
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

    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 440.0),
        10);
    const auto plan = build_waveform_gpu_static_layer_plan(
        thumbnail,
        make_viewport(),
        make_config());
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

TEST_CASE("Waveform GPU upload plans require explicit source generation",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 440.0),
        10);

    const auto implicit = build_waveform_gpu_static_layer_plan(thumbnail, make_viewport());
    REQUIRE_FALSE(implicit.valid);
    REQUIRE_FALSE(implicit.upload_key.valid());

    const auto explicit_plan = build_waveform_gpu_static_layer_plan(
        thumbnail,
        make_viewport(),
        make_config(9));
    REQUIRE(explicit_plan.valid);
    REQUIRE(explicit_plan.upload_key.valid());
    REQUIRE(explicit_plan.upload_key.source_generation == 9);
}

TEST_CASE("Waveform GPU resource cache returns records for release-safe clearing",
          "[view][waveform][gpu]") {
    const auto thumbnail = WaveformOverview::build_from_buffer(
        make_sine(48000, 1000, 1, 440.0),
        10);
    const auto plan = build_waveform_gpu_static_layer_plan(
        thumbnail,
        make_viewport(),
        make_config());
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

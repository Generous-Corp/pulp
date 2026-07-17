#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/sample_asset.hpp>

#include <limits>
#include <type_traits>

using Catch::Matchers::WithinAbs;
using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::SampleAsset;
using pulp::audio::SampleAssetConfig;
using pulp::audio::SampleAssetView;
using pulp::audio::SampleStreamCacheService;
using pulp::audio::SampleStreamCacheSourceView;
using pulp::audio::SampleStreamSourceToken;

static_assert(std::is_trivially_copyable_v<SampleAssetView>);
static_assert(std::is_standard_layout_v<SampleAssetView>);

namespace {

SampleAssetConfig resident_config() {
    return {
        .asset = {10, 2},
        .source = {20, 3},
        .channels = 2,
        .total_frames = 4,
        .sample_rate = 48000,
        .preload_frames = 4,
    };
}

void fill_preload(Buffer<float>& preload) {
    for (std::size_t frame = 0; frame < preload.num_samples(); ++frame) {
        preload.channel(0)[frame] = static_cast<float>(frame + 1);
        preload.channel(1)[frame] = static_cast<float>(frame + 11);
    }
}

}  // namespace

TEST_CASE("Sample asset owns an immutable resident preload behind a trivial view",
          "[audio][sampler][sample-asset]") {
    Buffer<float> preload(2, 4);
    fill_preload(preload);

    SampleAsset asset;
    REQUIRE(asset.prepare(resident_config(), preload.view()));
    const auto first = asset.view();
    const auto second = asset.view();
    REQUIRE(first.valid());
    REQUIRE(first.fully_resident());
    REQUIRE_FALSE(first.has_stream_source);
    REQUIRE(first.asset.asset_id == 10);
    REQUIRE(first.asset.asset_generation == 2);
    REQUIRE(first.source.source_id == 20);
    REQUIRE(first.source.source_generation == 3);
    REQUIRE(first.channels == 2);
    REQUIRE(first.total_frames == 4);
    REQUIRE(first.sample_rate == 48000);
    REQUIRE(first.preload_frames == 4);
    REQUIRE(first.preload_channels == second.preload_channels);
    REQUIRE(first.preload_contains(3));
    REQUIRE_FALSE(first.preload_contains(4));
    REQUIRE(first.preload_channel_data(2) == nullptr);
    REQUIRE_THAT(first.preload_channel_data(0)[3], WithinAbs(4.0f, 1.0e-6f));
    REQUIRE_THAT(first.preload_channel_data(1)[0], WithinAbs(11.0f, 1.0e-6f));

    preload.channel(0)[3] = 99.0f;
    REQUIRE_THAT(first.preload_channel_data(0)[3], WithinAbs(4.0f, 1.0e-6f));
}

TEST_CASE("Sample asset preserves finite fractional sample rates",
          "[audio][sampler][sample-asset]") {
    Buffer<float> preload(2, 4);
    fill_preload(preload);
    auto config = resident_config();
    config.sample_rate = 5512.5;

    SampleAsset asset;
    REQUIRE(asset.prepare(config, preload.view()));
    REQUIRE(asset.view().sample_rate == 5512.5);

    config.sample_rate = std::numeric_limits<double>::infinity();
    REQUIRE_FALSE(asset.prepare(config, preload.view()));
    config.sample_rate = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_FALSE(asset.prepare(config, preload.view()));
}

TEST_CASE("Partial sample asset requires a matching prepared stream source",
          "[audio][sampler][sample-asset][streaming]") {
    SampleStreamCacheService service;
    REQUIRE(service.prepare({
        .scheduler_capacity = 4,
        .page_memory_budget_bytes = 64,
    }));

    const SampleStreamSourceToken source{30, 4};
    const auto added = service.add_source({
        .token = source,
        .channels = 2,
        .total_frames = 12,
        .page_frames = 4,
        .cache_page_count = 2,
    }, [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t frames) {
        return frames;
    });
    REQUIRE(added.added());
    const auto stream = added.view;
    SampleAssetConfig config{
        .asset = {11, 1},
        .source = source,
        .channels = 2,
        .total_frames = 12,
        .sample_rate = 44100,
        .preload_frames = 4,
        .preload_contract = pulp::audio::SamplePreloadContract{
            .source_sample_rate = 44100,
            .host_sample_rate = 48000,
            .maximum_playback_ratio = 1.0,
            .maximum_host_block_frames = 1,
            .interpolation_guard_frames = 1,
            .configured_preload_frames = 4,
        },
        .stream_source = stream,
    };
    Buffer<float> preload(2, 4);
    fill_preload(preload);

    SampleAsset asset;
    REQUIRE(asset.prepare(config, preload.view()));
    const auto view = asset.view();
    REQUIRE(view.valid());
    REQUIRE_FALSE(view.fully_resident());
    REQUIRE(view.has_stream_source);
    REQUIRE(view.stream_source.window == stream.window);
    REQUIRE(view.stream_source.token.source_id == source.source_id);

    config.sample_rate = 5512.5;
    config.preload_contract->source_sample_rate = 5512.5;
    REQUIRE(asset.prepare(config, preload.view()));
    REQUIRE(asset.view().sample_rate == 5512.5);
    auto tampered_fractional_view = asset.view();
    tampered_fractional_view.sample_rate = 5513.0;
    REQUIRE_FALSE(tampered_fractional_view.valid());
    config.sample_rate = 44100.0;
    config.preload_contract->source_sample_rate = 44100.0;

    config.stream_source.reset();
    REQUIRE_FALSE(asset.prepare(config, preload.view()));
    REQUIRE_FALSE(asset.prepared());

    config.stream_source = stream;
    config.stream_source->token.source_generation += 1;
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config.stream_source = stream;
    config.stream_source->page_frames += 1;
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config.stream_source = stream;
    config.preload_contract->configured_preload_frames = 3;
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config.preload_contract->configured_preload_frames = 4;
    config.preload_contract->maximum_host_block_frames = 8;
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config.preload_contract.reset();
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config.preload_contract = pulp::audio::SamplePreloadContract{
        .source_sample_rate = 44100,
        .host_sample_rate = 48000,
        .maximum_playback_ratio = 1.0,
        .maximum_host_block_frames = 1,
        .interpolation_guard_frames = 1,
        .configured_preload_frames = 4,
    };
    config.stream_source = SampleStreamCacheSourceView{
        .token = source,
        .window = stream.window,
        .total_frames = 12,
        .page_frames = 4,
    };
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    auto forged = view;
    forged.stream_source.total_frames += 1;
    REQUIRE_FALSE(forged.valid());

    forged = view;
    forged.total_frames += 1;
    forged.stream_source.total_frames += 1;
    REQUIRE_FALSE(forged.valid());

    forged = view;
    forged.preload_contract.maximum_host_block_frames += 1;
    REQUIRE_FALSE(forged.valid());
}

TEST_CASE("Sample asset rejects malformed identity metadata and preload bounds",
          "[audio][sampler][sample-asset][edge]") {
    Buffer<float> preload(2, 4);
    fill_preload(preload);
    SampleAsset asset;

    auto config = resident_config();
    config.asset.asset_id = 0;
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config = resident_config();
    config.source.source_generation = 0;
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config = resident_config();
    config.sample_rate = 0;
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config = resident_config();
    config.preload_frames = 0;
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config = resident_config();
    config.preload_frames = config.total_frames + 1;
    REQUIRE_FALSE(asset.prepare(config, preload.view()));

    config = resident_config();
    Buffer<float> short_preload(2, 3);
    REQUIRE_FALSE(asset.prepare(config, short_preload.view()));

    config = resident_config();
    Buffer<float> mono_preload(1, 4);
    REQUIRE_FALSE(asset.prepare(config, mono_preload.view()));

    float* malformed_channels[] = {preload.channel(0).data(), nullptr};
    BufferView<const float> malformed(malformed_channels, 2, 4);
    REQUIRE_FALSE(asset.prepare(config, malformed));
}

TEST_CASE("Sample asset release invalidates the owner snapshot",
          "[audio][sampler][sample-asset][lifetime]") {
    Buffer<float> preload(2, 4);
    fill_preload(preload);
    SampleAsset asset;
    REQUIRE(asset.prepare(resident_config(), preload.view()));
    REQUIRE(asset.prepared());

    asset.release();
    REQUIRE_FALSE(asset.prepared());
    REQUIRE_FALSE(asset.view().valid());
    REQUIRE_FALSE(asset.view().fully_resident());
}

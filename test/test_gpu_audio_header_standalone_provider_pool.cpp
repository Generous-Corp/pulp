// The public header comes first so this translation unit proves it compiles on
// its own, with nothing included ahead of it.
#include <pulp/gpu_audio/gpu_audio_provider_pool.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

TEST_CASE("gpu_audio_provider_pool.hpp compiles as the first include", "[gpu_audio][headers]") {
    using Pool = pulp::gpu_audio::GpuAudioProviderPool;
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<Pool>);
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<Pool>);
    Pool pool;
    CHECK(pool.stats().warm_entries == 0);
    pool.evict_expired();
    pool.clear();
    CHECK(pool.stats().evictions == 0);
}

TEST_CASE("GpuAudioProviderPool keeps a provider warm for 30 s by default",
          "[gpu_audio][provider_pool]") {
    const pulp::gpu_audio::GpuAudioProviderPool::Config config;
    CHECK(config.keep_warm_ns == 30'000'000'000ull);
    CHECK(config.capacity == 4u);
    CHECK(config.release_budget_ns == 250'000'000ull);
}

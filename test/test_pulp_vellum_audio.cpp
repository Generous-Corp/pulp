#include <pulp/vellum_audio/audio_display_provider.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("Vellum audio provider publishes complete meter and scope frames",
          "[pulp-vellum-audio]") {
    using Provider = pulp::vellum_audio::AudioDisplayProvider;
    Provider provider({
        {.name = "level", .unit = "dB", .shape = ::vellum::value::ChannelShape::meter},
        {.name = "scope", .shape = ::vellum::value::ChannelShape::vector},
    });

    const auto meter = provider.find("level", ::vellum::value::ChannelShape::meter);
    const auto scope = provider.find("scope", ::vellum::value::ChannelShape::vector);
    REQUIRE(meter);
    REQUIRE(scope);

    provider.publish_meter(*meter, std::array{0.1f, 0.2f}, std::array{0.4f, 0.8f});
    provider.publish_scope(*scope, std::array{-1.0f, -0.5f, 0.0f, 0.5f, 1.0f});

    std::array<float, 2> rms{};
    std::array<float, 2> peak{};
    REQUIRE(provider.read_meter(*meter, rms, peak) == 2);
    CHECK(rms == std::array{0.1f, 0.2f});
    CHECK(peak == std::array{0.4f, 0.8f});

    std::array<float, 8> samples{};
    REQUIRE(provider.read_vector(*scope, samples) == 5);
    CHECK(samples[0] == -1.0f);
    CHECK(samples[4] == 1.0f);
    CHECK(provider.publish_count(*meter) != 0);
    CHECK(provider.publish_count(*scope) != 0);
}

TEST_CASE("Vellum audio provider refuses wrong channel shapes", "[pulp-vellum-audio]") {
    using Provider = pulp::vellum_audio::AudioDisplayProvider;
    Provider provider({{.name = "meter", .shape = ::vellum::value::ChannelShape::meter}});
    float value = 7.0f;
    CHECK_FALSE(provider.read_scalar(0, value));
    CHECK(provider.read_vector(0, std::span<float>{&value, 1}) == 0);
    CHECK(provider.publish_count(99) == 0);
}

TEST_CASE("Vellum audio provider never exposes a torn frame", "[pulp-vellum-audio][threading]") {
    using Provider = pulp::vellum_audio::AudioDisplayProvider;
    Provider provider({
        {.name = "meter", .shape = ::vellum::value::ChannelShape::meter},
        {.name = "scope", .shape = ::vellum::value::ChannelShape::vector},
    });
    const auto meter = provider.find("meter", ::vellum::value::ChannelShape::meter);
    const auto scope = provider.find("scope", ::vellum::value::ChannelShape::vector);
    constexpr std::uint32_t kIterations = 100'000;
    std::atomic<bool> done = false;
    std::atomic<bool> torn = false;

    std::thread audio([&] {
        for (std::uint32_t sequence = 1; sequence <= kIterations; ++sequence) {
            const float marker = static_cast<float>(sequence);
            const std::array rms{marker, marker, marker, marker};
            const std::array peak{marker + 0.25f, marker + 0.25f, marker + 0.25f, marker + 0.25f};
            const std::array scope_frame{marker + 0.5f, marker + 0.5f, marker + 0.5f,
                                         marker + 0.5f};
            provider.publish_meter(*meter, rms, peak);
            provider.publish_scope(*scope, scope_frame);
        }
        done.store(true, std::memory_order_release);
    });
    std::thread editor([&] {
        std::array<float, 8> rms{}, peak{}, samples{};
        while (!done.load(std::memory_order_acquire)) {
            const auto count = provider.read_meter(*meter, rms, peak);
            for (std::size_t i = 1; i < count; ++i)
                if (rms[i] != rms[0] || peak[i] != peak[0] || peak[i] != rms[0] + 0.25f)
                    torn.store(true, std::memory_order_relaxed);
            const auto sample_count = provider.read_vector(*scope, samples);
            for (std::size_t i = 1; i < sample_count; ++i)
                if (samples[i] != samples[0])
                    torn.store(true, std::memory_order_relaxed);
        }
    });
    audio.join();
    editor.join();

    std::array<float, 8> rms{}, peak{}, samples{};
    REQUIRE(provider.read_meter(*meter, rms, peak) == 4);
    REQUIRE(provider.read_vector(*scope, samples) == 4);
    CHECK_FALSE(torn.load(std::memory_order_relaxed));
    CHECK(rms[0] == static_cast<float>(kIterations));
    CHECK(peak[0] == static_cast<float>(kIterations) + 0.25f);
    CHECK(samples[0] == static_cast<float>(kIterations) + 0.5f);
}

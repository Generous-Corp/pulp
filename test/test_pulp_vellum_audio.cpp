#include <pulp/vellum_audio/audio_display_provider.hpp>
#include <pulp/vellum_audio/audio_widgets.hpp>

#include <vellum/view/screenshot.hpp>

#include <catch2/catch_test_macros.hpp>

#include "support/thread_progress.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace {

struct RenderedFrame {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

RenderedFrame render(::vellum::view::View& root) {
    RenderedFrame frame;
    frame.pixels = ::vellum::view::render_to_rgba(root, 128, 96, 1.0f, &frame.width, &frame.height);
    return frame;
}

void require_feed_render_contract(const RenderedFrame& first, const RenderedFrame& second,
                                  const RenderedFrame& repeat, std::uint32_t widget_x,
                                  std::uint32_t widget_y, std::uint32_t widget_width,
                                  std::uint32_t widget_height) {
    REQUIRE(first.width == second.width);
    REQUIRE(first.height == second.height);
    REQUIRE(first.pixels.size() == second.pixels.size());
    REQUIRE(first.pixels.size() == repeat.pixels.size());
    CHECK(first.pixels != second.pixels);
    CHECK(second.pixels == repeat.pixels);

    bool changed = false;
    for (std::uint32_t y = 0; y < first.height; ++y) {
        for (std::uint32_t x = 0; x < first.width; ++x) {
            const auto offset = static_cast<std::size_t>(y) * first.width * 4 + x * 4;
            const bool pixel_changed =
                !std::equal(first.pixels.begin() + offset, first.pixels.begin() + offset + 4,
                            second.pixels.begin() + offset);
            if (pixel_changed)
                changed = true;
            if (pixel_changed && (x < widget_x || x >= widget_x + widget_width || y < widget_y ||
                                  y >= widget_y + widget_height))
                FAIL_CHECK("feed changed a pixel outside the widget bounds");
        }
    }
    CHECK(changed);
}

} // namespace

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

TEST_CASE("Vellum audio provider read_meter returns the count actually copied",
          "[pulp-vellum-audio]") {
    pulp::vellum_audio::AudioDisplayProvider provider({
        {.name = "level", .shape = ::vellum::value::ChannelShape::meter},
    });
    const auto meter = provider.find("level", ::vellum::value::ChannelShape::meter);
    REQUIRE(meter);
    provider.publish_meter(*meter, std::array{0.1f, 0.2f, 0.3f}, std::array{0.4f, 0.5f, 0.6f});

    std::array<float, 1> short_rms{};
    std::array<float, 3> long_peak{};
    CHECK(provider.read_meter(*meter, short_rms, long_peak) == 1);
    CHECK(short_rms[0] == 0.1f);

    std::array<float, 3> long_rms{};
    std::array<float, 2> short_peak{};
    CHECK(provider.read_meter(*meter, long_rms, short_peak) == 2);
    CHECK(short_peak[1] == 0.5f);
}

TEST_CASE("Vellum audio provider refuses wrong channel shapes", "[pulp-vellum-audio]") {
    using Provider = pulp::vellum_audio::AudioDisplayProvider;
    Provider provider({{.name = "meter", .shape = ::vellum::value::ChannelShape::meter}});
    float value = 7.0f;
    CHECK_FALSE(provider.read_scalar(0, value));
    CHECK(provider.read_vector(0, std::span<float>{&value, 1}) == 0);
    CHECK(provider.publish_count(99) == 0);
}

TEST_CASE("Vellum audio feed reports stale and dropped-frame metadata", "[pulp-vellum-audio]") {
    using Provider = pulp::vellum_audio::AudioDisplayProvider;
    Provider provider({{.name = "scope", .shape = ::vellum::value::ChannelShape::vector}});
    const auto scope = provider.find("scope", ::vellum::value::ChannelShape::vector);
    REQUIRE(scope);
    pulp::vellum_audio::AudioFrameInfo info;
    CHECK_FALSE(provider.read_info(*scope, info));

    const std::array first{-90.0f, -80.0f, -70.0f};
    info.viewport_min_hz = 30.0f;
    info.viewport_max_hz = 18000.0f;
    info.floor_db = -96.0f;
    info.sample_rate = 44100.0;
    info.epoch = 7;
    info.sequence = 12;
    info.dropped_frames = 3;
    provider.publish_scope(*scope, first, info);

    pulp::vellum_audio::AudioFrameInfo observed;
    REQUIRE(provider.read_info(*scope, observed));
    CHECK(observed.viewport_min_hz == 30.0f);
    CHECK(observed.viewport_max_hz == 18000.0f);
    CHECK(observed.floor_db == -96.0f);
    CHECK(observed.sample_rate == 44100.0);
    CHECK(observed.epoch == 7);
    CHECK(observed.sequence == 12);
    CHECK(observed.dropped_frames == 3);
}

TEST_CASE("Vellum audio widgets snapshot feed values", "[pulp-vellum-audio]") {
    pulp::vellum_audio::AudioDisplayProvider provider({
        {.name = "level", .shape = ::vellum::value::ChannelShape::meter},
        {.name = "spectrum", .shape = ::vellum::value::ChannelShape::vector},
    });
    const auto level = provider.find("level", ::vellum::value::ChannelShape::meter);
    const auto spectrum = provider.find("spectrum", ::vellum::value::ChannelShape::vector);
    REQUIRE(level);
    REQUIRE(spectrum);

    pulp::vellum_audio::LevelMeter meter(&provider);
    meter.set_channel(*level);
    meter.sample();
    CHECK(meter.stale());
    provider.publish_meter(*level, std::array{0.25f}, std::array{0.75f});
    meter.sample();
    CHECK_FALSE(meter.stale());
    CHECK(meter.rms() == 0.25f);
    CHECK(meter.peak() == 0.75f);

    pulp::vellum_audio::AnalyzerScope scope(&provider);
    scope.set_channel(*spectrum);
    pulp::signal::SpectrumBinMappingConfig mapping;
    mapping.fft_size = 4;
    mapping.columns = 3;
    mapping.min_hz = 20.0;
    mapping.max_hz = 18000.0;
    mapping.floor_db = -96.0f;
    scope.configure_mapping(mapping);
    provider.publish_scope(*spectrum, std::array{-90.0f, -60.0f, -30.0f},
                           {.floor_db = -96.0f, .sequence = 2});
    scope.sample();
    CHECK_FALSE(scope.stale());
    CHECK(scope.column_count() == 3);
}

TEST_CASE("Vellum AnalyzerScope keeps its mapping across stale periods and set_feed",
          "[pulp-vellum-audio]") {
    pulp::vellum_audio::AudioDisplayProvider provider({
        {.name = "spectrum", .shape = ::vellum::value::ChannelShape::vector},
    });
    const auto spectrum = provider.find("spectrum", ::vellum::value::ChannelShape::vector);
    REQUIRE(spectrum);
    pulp::vellum_audio::AnalyzerScope scope(&provider);
    scope.set_channel(*spectrum);
    pulp::signal::SpectrumBinMappingConfig mapping;
    mapping.fft_size = 4;
    mapping.columns = 3;
    mapping.min_hz = 20.0;
    mapping.max_hz = 18000.0;
    mapping.floor_db = -96.0f;
    scope.configure_mapping(mapping);
    provider.publish_scope(*spectrum, std::array{-90.0f, -60.0f, -30.0f},
                           {.floor_db = -96.0f, .sequence = 1});
    scope.sample();
    REQUIRE(scope.column_count() == 3);

    for (int i = 0; i < 8; ++i)
        scope.sample(); // quiet feed goes stale
    CHECK(scope.stale());

    provider.publish_scope(*spectrum, std::array{-80.0f, -50.0f, -20.0f},
                           {.floor_db = -96.0f, .sequence = 2});
    scope.sample();
    CHECK_FALSE(scope.stale());
    CHECK(scope.column_count() == 3);

    scope.set_feed(&provider, *spectrum);
    provider.publish_scope(*spectrum, std::array{-70.0f, -40.0f, -10.0f},
                           {.floor_db = -96.0f, .sequence = 3});
    scope.sample();
    CHECK_FALSE(scope.stale());
    CHECK(scope.column_count() == 3);
}

TEST_CASE("Vellum audio widgets resample after set_channel", "[pulp-vellum-audio]") {
    pulp::vellum_audio::AudioDisplayProvider provider({
        {.name = "level_a", .shape = ::vellum::value::ChannelShape::meter},
        {.name = "level_b", .shape = ::vellum::value::ChannelShape::meter},
        {.name = "spectrum_a", .shape = ::vellum::value::ChannelShape::vector},
        {.name = "spectrum_b", .shape = ::vellum::value::ChannelShape::vector},
    });
    const auto level_a = provider.find("level_a", ::vellum::value::ChannelShape::meter);
    const auto level_b = provider.find("level_b", ::vellum::value::ChannelShape::meter);
    const auto spectrum_a = provider.find("spectrum_a", ::vellum::value::ChannelShape::vector);
    const auto spectrum_b = provider.find("spectrum_b", ::vellum::value::ChannelShape::vector);
    REQUIRE(level_a);
    REQUIRE(level_b);
    REQUIRE(spectrum_a);
    REQUIRE(spectrum_b);

    // Both channels publish once, so their publish counts are equal: a stale
    // sampling cursor would treat the new channel as unchanged.
    provider.publish_meter(*level_a, std::array{0.25f}, std::array{0.5f});
    provider.publish_meter(*level_b, std::array{0.75f}, std::array{0.9f});
    REQUIRE(provider.publish_count(*level_a) == provider.publish_count(*level_b));

    pulp::vellum_audio::LevelMeter meter(&provider);
    meter.set_channel(*level_a);
    meter.sample();
    REQUIRE(meter.rms() == 0.25f);
    meter.set_channel(*level_b);
    meter.sample();
    CHECK_FALSE(meter.stale());
    CHECK(meter.rms() == 0.75f);
    CHECK(meter.peak() == 0.9f);

    provider.publish_scope(*spectrum_a, std::array{-90.0f, -60.0f, -30.0f},
                           {.floor_db = -96.0f, .sequence = 1});
    provider.publish_scope(*spectrum_b, std::array{-80.0f, -50.0f, -20.0f},
                           {.floor_db = -96.0f, .sequence = 7});
    REQUIRE(provider.publish_count(*spectrum_a) == provider.publish_count(*spectrum_b));

    pulp::vellum_audio::AnalyzerScope scope(&provider);
    scope.set_channel(*spectrum_a);
    scope.sample();
    REQUIRE(scope.frame_info().sequence == 1);
    scope.set_channel(*spectrum_b);
    scope.sample();
    CHECK_FALSE(scope.stale());
    CHECK(scope.frame_info().sequence == 7);
}

TEST_CASE("Vellum LevelMeter matches legacy thresholds, ballistics, hold, and accessibility",
          "[pulp-vellum-audio][level-meter]") {
    pulp::vellum_audio::LevelMeter meter;
    const auto green = meter.level_color(0.5f);
    const auto yellow = meter.level_color(0.8f);
    const auto red = meter.level_color(0.95f);
    CHECK(green.r < yellow.r);
    CHECK(green.g > yellow.g);
    CHECK(red.g < yellow.g);
    meter.set_levels(0.2f, 0.95f);
    CHECK(meter.displayed_peak() > 0.0f);
    CHECK(meter.held_peak() == 0.95f);
    CHECK(meter.get_current_value() == meter.displayed_peak());
    const auto held = meter.held_peak();
    for (int i = 0; i < 120; ++i)
        meter.set_levels(0.0f, 0.0f);
    CHECK(meter.displayed_peak() < 0.01f);
    CHECK(meter.held_peak() < held);
}

TEST_CASE("Vellum MidiKeyboard hit-tests black keys and emits editor MIDI callbacks",
          "[pulp-vellum-audio][midi-keyboard]") {
    pulp::vellum_audio::MidiKeyboard keyboard;
    keyboard.set_range(60, 64);
    keyboard.set_bounds({0, 0, 500, 100});
    std::vector<std::pair<int, float>> ons;
    std::vector<int> offs;
    keyboard.on_note_on = [&](int note, float velocity) { ons.emplace_back(note, velocity); };
    keyboard.on_note_off = [&](int note) { offs.push_back(note); };

    keyboard.on_mouse_down({175.0f, 10.0f}); // C# overlaps the C/D naturals
    REQUIRE(ons.size() == 1);
    CHECK(ons.front().first == 61);
    CHECK(ons.front().second == 0.8f);
    keyboard.on_mouse_drag({450.0f, 80.0f});
    REQUIRE(offs.size() == 1);
    CHECK(offs.front() == 61);
    REQUIRE(ons.size() == 2);
    CHECK(ons.back().first == 64);
    keyboard.on_mouse_up({450.0f, 80.0f});
    REQUIRE(offs.size() == 2);
    CHECK(offs.back() == 64);
}

TEST_CASE("Vellum LevelMeter feed changes are rendered inside its bounds",
          "[pulp-vellum-audio][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    pulp::vellum_audio::AudioDisplayProvider provider({
        {.name = "level", .shape = ::vellum::value::ChannelShape::meter},
    });
    const auto level = provider.find("level", ::vellum::value::ChannelShape::meter);
    REQUIRE(level);

    pulp::vellum_audio::LevelMeter meter(&provider);
    meter.set_channel(*level);
    meter.set_bounds({0, 0, 128, 96});

    provider.publish_meter(*level, std::array{0.15f}, std::array{0.25f});
    meter.sample();
    REQUIRE(meter.rms() == 0.15f);
    const auto first = render(meter);
    provider.publish_meter(*level, std::array{0.75f}, std::array{0.9f});
    meter.sample();
    REQUIRE(meter.rms() == 0.75f);
    const auto second = render(meter);
    const auto repeat = render(meter);

    require_feed_render_contract(first, second, repeat, 0, 0, 128, 96);
}

TEST_CASE("Vellum AnalyzerScope feed changes are rendered inside its bounds",
          "[pulp-vellum-audio][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    pulp::vellum_audio::AudioDisplayProvider provider({
        {.name = "spectrum", .shape = ::vellum::value::ChannelShape::vector},
    });
    const auto spectrum = provider.find("spectrum", ::vellum::value::ChannelShape::vector);
    REQUIRE(spectrum);

    pulp::vellum_audio::AnalyzerScope scope(&provider);
    scope.set_channel(*spectrum);
    scope.set_bounds({0, 0, 128, 96});
    pulp::signal::SpectrumBinMappingConfig mapping;
    // bin_count() is fft_size / 2 + 1, so fft_size 6 matches the 4-bin frames
    // below; a mismatched frame is dropped and the scope would stay stale.
    mapping.fft_size = 6;
    mapping.columns = 4;
    mapping.min_hz = 20.0;
    mapping.max_hz = 18000.0;
    mapping.floor_db = -96.0f;
    scope.configure_mapping(mapping);

    provider.publish_scope(*spectrum, std::array{-90.0f, -70.0f, -50.0f, -30.0f},
                           {.floor_db = -96.0f, .sequence = 1});
    scope.sample();
    REQUIRE_FALSE(scope.stale());
    const auto first = render(scope);
    provider.publish_scope(*spectrum, std::array{-80.0f, -45.0f, -20.0f, -5.0f},
                           {.floor_db = -96.0f, .sequence = 2});
    scope.sample();
    REQUIRE_FALSE(scope.stale());
    const auto second = render(scope);
    const auto repeat = render(scope);

    require_feed_render_contract(first, second, repeat, 0, 0, 128, 96);
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
        const auto deadline = std::chrono::steady_clock::now() + pulp::test::kProgressDeadline;
        while (!done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
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
    REQUIRE(done.load(std::memory_order_acquire));

    std::array<float, 8> rms{}, peak{}, samples{};
    REQUIRE(provider.read_meter(*meter, rms, peak) == 4);
    REQUIRE(provider.read_vector(*scope, samples) == 4);
    CHECK_FALSE(torn.load(std::memory_order_relaxed));
    CHECK(rms[0] == static_cast<float>(kIterations));
    CHECK(peak[0] == static_cast<float>(kIterations) + 0.25f);
    CHECK(samples[0] == static_cast<float>(kIterations) + 0.5f);
}

TEST_CASE("Vellum audio provider reads values and frame info from one frame",
          "[pulp-vellum-audio][threading]") {
    using Provider = pulp::vellum_audio::AudioDisplayProvider;
    Provider provider({
        {.name = "meter", .shape = ::vellum::value::ChannelShape::meter},
        {.name = "scope", .shape = ::vellum::value::ChannelShape::vector},
    });
    const auto meter = provider.find("meter", ::vellum::value::ChannelShape::meter);
    const auto scope = provider.find("scope", ::vellum::value::ChannelShape::vector);
    REQUIRE(meter);
    REQUIRE(scope);
    constexpr std::uint32_t kIterations = 100'000;
    std::atomic<bool> done = false;
    std::atomic<bool> mismatched = false;

    // Each frame's values equal its sequence, so a value paired with another
    // frame's metadata shows up as values != info.sequence.
    std::thread audio([&] {
        for (std::uint32_t sequence = 1; sequence <= kIterations; ++sequence) {
            const float marker = static_cast<float>(sequence);
            const std::array values{marker, marker};
            provider.publish_meter(*meter, values, values, {.sequence = sequence});
            provider.publish_scope(*scope, values, {.sequence = sequence});
        }
        done.store(true, std::memory_order_release);
    });
    std::thread editor([&] {
        std::array<float, 2> rms{}, peak{}, samples{};
        const auto deadline = std::chrono::steady_clock::now() + pulp::test::kProgressDeadline;
        while (!done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            pulp::vellum_audio::AudioFrameInfo meter_info;
            if (provider.read_meter(*meter, rms, peak, meter_info) != 0 &&
                rms[0] != static_cast<float>(meter_info.sequence))
                mismatched.store(true, std::memory_order_relaxed);
            pulp::vellum_audio::AudioFrameInfo scope_info;
            if (provider.read_vector(*scope, samples, scope_info) != 0 &&
                samples[0] != static_cast<float>(scope_info.sequence))
                mismatched.store(true, std::memory_order_relaxed);
        }
    });
    audio.join();
    editor.join();
    REQUIRE(done.load(std::memory_order_acquire));
    CHECK_FALSE(mismatched.load(std::memory_order_relaxed));

    std::array<float, 2> rms{}, peak{};
    pulp::vellum_audio::AudioFrameInfo info;
    REQUIRE(provider.read_meter(*meter, rms, peak, info) == 2);
    CHECK(info.sequence == kIterations);
    CHECK(rms[0] == static_cast<float>(kIterations));
}

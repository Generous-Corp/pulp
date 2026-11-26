#include <pulp/vellum_audio/eq_curve_view.hpp>
#include <pulp/vellum_audio/graph_scale.hpp>

#include <pulp/signal/frequency_response.hpp>
#include <pulp/state/store.hpp>

#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/continuous_frames.hpp>
#include <vellum/view/screenshot.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace pulp::vellum_audio;
using Catch::Matchers::WithinAbs;
using ::vellum::canvas::Color;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;
using ::vellum::view::GestureEvent;
using ::vellum::view::GesturePhase;
using ::vellum::view::MouseButton;
using ::vellum::view::MouseEvent;
using ::vellum::view::needs_continuous_frames;
using ::vellum::view::Point;

namespace {

bool has_text(const RecordingCanvas& canvas, std::string_view text) {
    return std::any_of(
        canvas.commands().begin(), canvas.commands().end(), [&](const DrawCommand& command) {
            return command.type == DrawCommand::Type::fill_text && command.text == text;
        });
}

bool has_fill_color(const RecordingCanvas& canvas, Color color) {
    return std::any_of(
        canvas.commands().begin(), canvas.commands().end(), [&](const DrawCommand& command) {
            return command.type == DrawCommand::Type::set_fill_color && command.color == color;
        });
}

} // namespace

TEST_CASE("EqCurveView bind_bands records a dot drag as one gesture", "[eq-curve][automation]") {
    using pulp::state::ListenerThread;
    using pulp::state::ParamID;
    using pulp::state::StateStore;

    StateStore store;
    store.add_parameter({.id = 1, .name = "F0", .range = {20.0f, 20000.0f, 1000.0f, 0.0f}});
    store.add_parameter({.id = 2, .name = "G0", .range = {-24.0f, 24.0f, 0.0f, 0.0f}});
    store.add_parameter({.id = 3, .name = "Q0", .range = {0.1f, 30.0f, 1.0f, 0.0f}});

    int begins = 0, ends = 0;
    store.set_gesture_callbacks([&](ParamID) { ++begins; }, [&](ParamID) { ++ends; });
    std::vector<std::pair<ParamID, float>> sets;
    auto rec = store.add_listener([&](ParamID id, float v) { sets.emplace_back(id, v); },
                                  ListenerThread::Audio);

    EqCurveView eq;
    eq.set_sample_rate(48000.0f);
    eq.set_frequency_range(20.0f, 20000.0f);
    eq.set_gain_range(-24.0f, 24.0f);
    eq.set_bounds({0, 0, 400, 300});
    // Low-pass: a dot drag moves ONLY frequency (no gain), so exactly one param
    // gesture opens and closes.
    eq.set_bands({{1000.0f, 0.0f, 0.707f, EqCurveView::FilterType::low_pass, true}});
    std::array<EqCurveView::BandParamIds, 1> ids{{{1, 2, 3}}};
    eq.bind_bands(store, std::span<const EqCurveView::BandParamIds>(ids.data(), ids.size()));

    const float hy = eq.gain_scale().to_y(0.0f); // low-pass handle rides 0 dB
    eq.on_mouse_down({eq.frequency_scale().to_x(1000.0f), hy});
    eq.on_mouse_drag({eq.frequency_scale().to_x(4000.0f), hy});
    eq.on_mouse_up({eq.frequency_scale().to_x(4000.0f), hy});

    REQUIRE(begins == 1); // exactly one begin_gesture (frequency only)
    REQUIRE(ends == 1);   // and exactly one end_gesture
    // At least one frequency set_value was emitted during the drag.
    REQUIRE(std::any_of(sets.begin(), sets.end(), [](const auto& s) { return s.first == 1u; }));
    // A gain-less filter writes neither gain nor Q.
    REQUIRE_FALSE(std::any_of(sets.begin(), sets.end(),
                              [](const auto& s) { return s.first == 2u || s.first == 3u; }));
    // The recorded band actually moved.
    REQUIRE(eq.bands()[0].frequency > 1000.0f);
}

TEST_CASE("EqCurveView bind_bands records scroll-Q and reset as gestures",
          "[eq-curve][automation]") {
    using pulp::state::ListenerThread;
    using pulp::state::ParamID;
    using pulp::state::StateStore;

    StateStore store;
    store.add_parameter({.id = 1, .name = "F0", .range = {20.0f, 20000.0f, 1000.0f, 0.0f}});
    store.add_parameter({.id = 2, .name = "G0", .range = {-24.0f, 24.0f, 0.0f, 0.0f}});
    store.add_parameter({.id = 3, .name = "Q0", .range = {0.1f, 30.0f, 1.0f, 0.0f}});

    int begins = 0, ends = 0;
    store.set_gesture_callbacks([&](ParamID) { ++begins; }, [&](ParamID) { ++ends; });
    std::vector<std::pair<ParamID, float>> sets;
    auto rec = store.add_listener([&](ParamID id, float v) { sets.emplace_back(id, v); },
                                  ListenerThread::Audio);

    EqCurveView eq;
    eq.set_sample_rate(48000.0f);
    eq.set_frequency_range(20.0f, 20000.0f);
    eq.set_gain_range(-24.0f, 24.0f);
    eq.set_bounds({0, 0, 400, 300});
    eq.set_bands({{1000.0f, 6.0f, 1.0f, EqCurveView::FilterType::peak, true}});
    std::array<EqCurveView::BandParamIds, 1> ids{{{1, 2, 3}}};
    eq.bind_bands(store, std::span<const EqCurveView::BandParamIds>(ids.data(), ids.size()));

    const float hx = eq.frequency_scale().to_x(1000.0f);
    const float hy = eq.gain_scale().to_y(6.0f); // peak handle rides its gain

    SECTION("scroll wheel over a dot records a Q gesture") {
        MouseEvent e;
        e.is_wheel = true;
        e.position = {hx, hy};
        e.scroll_delta_y = -6.0f; // narrow → raise Q
        eq.on_mouse_event(e);

        REQUIRE(begins == 1);
        REQUIRE(ends == 1);
        REQUIRE(std::any_of(sets.begin(), sets.end(),
                            [](const auto& s) { return s.first == 3u; })); // Q written
        REQUIRE(eq.bands()[0].q > 1.0f);
    }

    SECTION("double-click a dot resets its gain in one gesture") {
        MouseEvent e;
        e.is_down = true;
        e.click_count = 2;
        e.position = {hx, hy};
        eq.on_mouse_event(e);

        REQUIRE(begins == 1);
        REQUIRE(ends == 1);
        bool gain_reset = std::any_of(sets.begin(), sets.end(), [](const auto& s) {
            return s.first == 2u && s.second == 0.0f;
        });
        REQUIRE(gain_reset);
        REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(0.0f, 0.001f));
    }
}

TEST_CASE("EqCurveView bind_bands plays back a host write without re-recording",
          "[eq-curve][automation]") {
    using pulp::state::ListenerThread;
    using pulp::state::ParamID;
    using pulp::state::StateStore;

    StateStore store;
    store.add_parameter({.id = 1, .name = "F0", .range = {20.0f, 20000.0f, 1000.0f, 0.0f}});
    store.add_parameter({.id = 2, .name = "G0", .range = {-24.0f, 24.0f, 0.0f, 0.0f}});
    store.add_parameter({.id = 3, .name = "Q0", .range = {0.1f, 30.0f, 1.0f, 0.0f}});

    int begins = 0, ends = 0;
    store.set_gesture_callbacks([&](ParamID) { ++begins; }, [&](ParamID) { ++ends; });

    EqCurveView eq;
    eq.set_sample_rate(48000.0f);
    eq.set_bounds({0, 0, 400, 300});
    eq.set_bands({{1000.0f, 3.0f, 1.0f, EqCurveView::FilterType::peak, true}});
    std::array<EqCurveView::BandParamIds, 1> ids{{{1, 2, 3}}};
    eq.bind_bands(store, std::span<const EqCurveView::BandParamIds>(ids.data(), ids.size()));

    // The host automates freq + gain from the audio thread; the UI pump drains it.
    store.set_value_rt(1, 5000.0f);
    store.set_value_rt(2, -6.0f);
    const std::size_t drained = store.pump_listeners();

    REQUIRE(drained >= 1);
    REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(5000.0f, 0.01f));
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(-6.0f, 0.01f));
    // The playback listener is a pure widget update — it must NOT open a gesture
    // nor re-emit an edit back to the store.
    REQUIRE(begins == 0);
    REQUIRE(ends == 0);
}

TEST_CASE("EqCurveView analyzer scale spans full plot height", "[eq-curve][analyzer]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 400, 300}); // plot: y 0 (top) .. 300 (bottom)

    // Default 0 dBFS → top, −60 dBFS → bottom, linear in between.
    REQUIRE_THAT(eq.analyzer_db_to_y(0.0f), WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(eq.analyzer_db_to_y(-60.0f), WithinAbs(300.0f, 0.01f));
    REQUIRE_THAT(eq.analyzer_db_to_y(-30.0f), WithinAbs(150.0f, 0.01f));

    // Independent of the ±gain axis: narrowing the gain range must not move it.
    eq.set_gain_range(-6.0f, 6.0f);
    REQUIRE_THAT(eq.analyzer_db_to_y(-30.0f), WithinAbs(150.0f, 0.01f));

    // Clamped to the plot at both ends.
    REQUIRE_THAT(eq.analyzer_db_to_y(12.0f), WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(eq.analyzer_db_to_y(-120.0f), WithinAbs(300.0f, 0.01f));

    // The setters retarget the scale (reserved for a future drag-to-zoom).
    eq.set_analyzer_range(-10.0f, -70.0f);
    REQUIRE_THAT(eq.analyzer_db_to_y(-10.0f), WithinAbs(0.0f, 0.01f));
    REQUIRE_THAT(eq.analyzer_db_to_y(-70.0f), WithinAbs(300.0f, 0.01f));
}

TEST_CASE("EqCurveView analyzer renders full-height (headless PNG)",
          "[eq-curve][analyzer][render]") {
    const char* png_dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR");
    if (png_dir == nullptr)
        return;

    EqCurveView eq;
    eq.set_sample_rate(48000.0f);
    eq.set_frequency_range(20.0f, 24000.0f);
    eq.set_gain_range(-12.0f, 12.0f);
    eq.set_bounds({0, 0, 900, 400});
    eq.set_bands({
        {80.0f, 3.0f, 0.7f, EqCurveView::FilterType::low_shelf, true},
        {240.0f, -4.5f, 1.4f, EqCurveView::FilterType::peak, true},
        {1000.0f, 6.0f, 2.4f, EqCurveView::FilterType::peak, true},
        {3200.0f, 2.0f, 1.2f, EqCurveView::FilterType::peak, true},
        {6400.0f, -3.0f, 1.6f, EqCurveView::FilterType::peak, true},
        {12000.0f, 4.0f, 0.7f, EqCurveView::FilterType::high_shelf, true},
    });
    // A broadband envelope near −40 dBFS with a low-frequency hump, so the fill
    // sits well into the plot and is clearly not crushed onto the gain axis.
    std::vector<float> mags(1025, -42.0f);
    for (std::size_t i = 8; i < 48; ++i)
        mags[i] = -18.0f;
    eq.set_spectrum(mags.data(), mags.size());

    const bool ok = ::vellum::view::render_to_file(
        eq, 900, 400, std::string(png_dir) + "/eq-analyzer-fullheight.png", 2.0f,
        ::vellum::view::ScreenshotBackend::skia);
    REQUIRE(ok);
}

TEST_CASE("EqCurveView is analyzer-enabled by default and gates frames on data",
          "[eq-curve][analyzer]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 400, 300});

    // Default: analyzer-capable, drag-to-zoom active, but idle (no data yet).
    REQUIRE(eq.analyzer_enabled());
    REQUIRE(eq.analyzer_scale_draggable());
    REQUIRE_FALSE(eq.analyzer_animating());
    REQUIRE_FALSE(needs_continuous_frames(&eq));

    // Binding a spectrum makes it animate (and the widget joins continuous frames).
    std::vector<float> mags(128, -40.0f);
    eq.set_spectrum(mags.data(), mags.size());
    REQUIRE(eq.analyzer_animating());
    REQUIRE(needs_continuous_frames(&eq));

    // Disabling the analyzer parks it again even with stale data present.
    eq.set_analyzer_enabled(false);
    REQUIRE_FALSE(eq.analyzer_enabled());
    REQUIRE_FALSE(eq.analyzer_animating());
    REQUIRE_FALSE(needs_continuous_frames(&eq));

    // A disabled analyzer draws no overlay even though smoothed data remains: the
    // envelope contributes one gradient fill, so the enabled paint records more
    // gradient commands than the disabled one (bands with no gain here → the
    // envelope is the only gradient in play).
    eq.set_bands({{1000.0f, 0.0f, 1.0f, EqCurveView::FilterType::low_pass, true}});
    eq.set_spectrum(mags.data(), mags.size());
    eq.set_analyzer_enabled(true);
    RecordingCanvas on_canvas;
    eq.paint(on_canvas);
    eq.set_analyzer_enabled(false);
    RecordingCanvas off_canvas;
    eq.paint(off_canvas);
    REQUIRE(on_canvas.count(DrawCommand::Type::set_fill_gradient_linear) >
            off_canvas.count(DrawCommand::Type::set_fill_gradient_linear));
}

TEST_CASE("EqCurveView left-gutter drag zooms the analyzer floor", "[eq-curve][analyzer]") {
    EqCurveView eq;
    eq.set_sample_rate(48000.0f);
    eq.set_frequency_range(20.0f, 24000.0f);
    eq.set_gain_range(-12.0f, 12.0f);
    eq.set_bounds({0, 0, 900, 400});
    // A mid-frequency peak: its dot sits at x ≈ 496, well clear of the ~28 px
    // left gutter, so a gutter press cannot accidentally grab it.
    eq.set_bands({{1000.0f, 6.0f, 1.0f, EqCurveView::FilterType::peak, true}});
    const auto before = eq.bands()[0];

    // Default range.
    REQUIRE_THAT(eq.analyzer_top_db(), WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(eq.analyzer_bottom_db(), WithinAbs(-60.0f, 0.001f));

    SECTION("drag DOWN lowers the floor (zoom out) without moving the band") {
        eq.on_mouse_down({10.0f, 200.0f}); // empty press inside the left gutter
        eq.on_mouse_drag({10.0f, 300.0f}); // +100 px down
        eq.on_mouse_up({10.0f, 300.0f});
        // −60 − 100*0.35 = −95.
        REQUIRE_THAT(eq.analyzer_bottom_db(), WithinAbs(-95.0f, 0.5f));
        REQUIRE_THAT(eq.analyzer_top_db(), WithinAbs(0.0f, 0.001f)); // top pinned
        // The band is untouched.
        REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(before.frequency, 0.001f));
        REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(before.gain_db, 0.001f));
    }

    SECTION("drag UP raises the floor (zoom in)") {
        eq.on_mouse_down({10.0f, 300.0f});
        eq.on_mouse_drag({10.0f, 200.0f}); // −100 px up
        eq.on_mouse_up({10.0f, 200.0f});
        // −60 + 100*0.35 = −25.
        REQUIRE_THAT(eq.analyzer_bottom_db(), WithinAbs(-25.0f, 0.5f));
    }

    SECTION("the floor clamps at −100 (zoom out) and keeps the min span (zoom in)") {
        eq.on_mouse_down({10.0f, 10.0f});
        eq.on_mouse_drag({10.0f, 5000.0f}); // far past the floor
        eq.on_mouse_up({10.0f, 5000.0f});
        REQUIRE_THAT(eq.analyzer_bottom_db(), WithinAbs(-100.0f, 0.001f));

        eq.on_mouse_down({10.0f, 390.0f});
        eq.on_mouse_drag({10.0f, -5000.0f}); // far above the ceiling
        eq.on_mouse_up({10.0f, -5000.0f});
        // top (0) − min span (20) = −20; never inverts / collapses.
        REQUIRE_THAT(eq.analyzer_bottom_db(), WithinAbs(-20.0f, 0.001f));
        REQUIRE((eq.analyzer_top_db() - eq.analyzer_bottom_db()) >= 20.0f);
        REQUIRE(eq.analyzer_top_db() <= 20.0f);
    }

    SECTION("a dot over the gutter still drags the band, not the scale") {
        // A 20 Hz band's dot sits at the very left edge (x ≈ 0), inside the
        // gutter — pressing it must grab the dot, not start a zoom.
        eq.set_bands({{20.0f, 0.0f, 1.0f, EqCurveView::FilterType::peak, true}});
        const float hy = eq.gain_scale().to_y(0.0f); // dot at 0 dB
        eq.on_mouse_down({2.0f, hy});
        eq.on_mouse_drag({300.0f, hy - 40.0f});
        eq.on_mouse_up({300.0f, hy - 40.0f});
        // The analyzer range is untouched; the band moved instead.
        REQUIRE_THAT(eq.analyzer_bottom_db(), WithinAbs(-60.0f, 0.001f));
        REQUIRE(eq.bands()[0].frequency > 20.0f);
    }
}

TEST_CASE("EqCurveView analyzer scale-drag can be disabled", "[eq-curve][analyzer]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 900, 400});
    eq.set_analyzer_scale_draggable(false);
    eq.on_mouse_down({10.0f, 100.0f});
    eq.on_mouse_drag({10.0f, 300.0f});
    eq.on_mouse_up({10.0f, 300.0f});
    // No gutter drag happened → default range intact.
    REQUIRE_THAT(eq.analyzer_bottom_db(), WithinAbs(-60.0f, 0.001f));
}

TEST_CASE("EqCurveView analyzer default-on + zoom render (headless PNG)",
          "[eq-curve][analyzer][render]") {
    const char* png_dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR");
    if (png_dir == nullptr)
        return;

    auto make_eq = [] {
        auto eq = std::make_unique<EqCurveView>();
        eq->set_sample_rate(48000.0f);
        eq->set_frequency_range(20.0f, 24000.0f);
        eq->set_gain_range(-12.0f, 12.0f);
        eq->set_bounds({0, 0, 900, 400});
        eq->set_bands({
            {80.0f, 3.0f, 0.7f, EqCurveView::FilterType::low_shelf, true},
            {240.0f, -4.5f, 1.4f, EqCurveView::FilterType::peak, true},
            {1000.0f, 6.0f, 2.4f, EqCurveView::FilterType::peak, true},
            {3200.0f, 2.0f, 1.2f, EqCurveView::FilterType::peak, true},
            {6400.0f, -3.0f, 1.6f, EqCurveView::FilterType::peak, true},
            {12000.0f, 4.0f, 0.7f, EqCurveView::FilterType::high_shelf, true},
        });
        std::vector<float> mags(1025, -42.0f);
        for (std::size_t i = 8; i < 48; ++i)
            mags[i] = -18.0f;
        eq->set_spectrum(mags.data(), mags.size());
        return eq;
    };

    // 1) Analyzer on by default (no set_analyzer_enabled call needed).
    auto def = make_eq();
    REQUIRE(def->analyzer_enabled());
    REQUIRE(::vellum::view::render_to_file(*def, 900, 400,
                                           std::string(png_dir) + "/eq-analyzer-default.png", 2.0f,
                                           ::vellum::view::ScreenshotBackend::skia));

    // 2) Zoomed in: a simulated upward gutter drag raises the floor toward −40.
    auto zoom = make_eq();
    zoom->on_mouse_down({12.0f, 320.0f});
    zoom->on_mouse_drag({12.0f, 260.0f}); // +21 dB toward the floor (zoom in)
    zoom->on_mouse_up({12.0f, 260.0f});
    REQUIRE(zoom->analyzer_bottom_db() > -60.0f);
    REQUIRE(::vellum::view::render_to_file(*zoom, 900, 400,
                                           std::string(png_dir) + "/eq-analyzer-zoom.png", 2.0f,
                                           ::vellum::view::ScreenshotBackend::skia));
}

TEST_CASE("EqCurveView draws the true response, not a bell", "[eq-curve]") {
    EqCurveView eq;
    eq.set_sample_rate(48000.0f);

    SECTION("low-pass rolls off instead of peaking") {
        eq.set_bands({{1000.0f, 0.0f, 0.707f, EqCurveView::FilterType::low_pass, true}});

        // Flat below cutoff, -3 dB at it, and falling monotonically above —
        // none of which a symmetric bell centered on 1 kHz could produce.
        REQUIRE_THAT(eq.magnitude_db_at(100.0f), WithinAbs(0.0, 0.5));
        REQUIRE_THAT(eq.magnitude_db_at(1000.0f), WithinAbs(-3.0, 0.5));
        REQUIRE(eq.magnitude_db_at(4000.0f) < -20.0f);
        REQUIRE(eq.magnitude_db_at(4000.0f) > eq.magnitude_db_at(12000.0f));
    }

    SECTION("notch cuts a null at its center") {
        eq.set_bands({{1000.0f, 8.0f, 8.0f, EqCurveView::FilterType::notch, true}});
        REQUIRE(eq.magnitude_db_at(1000.0f) < -30.0f);
        REQUIRE_THAT(eq.magnitude_db_at(100.0f), WithinAbs(0.0, 0.5));
    }

    SECTION("low shelf plateaus down to DC") {
        eq.set_bands({{500.0f, 6.0f, 0.707f, EqCurveView::FilterType::low_shelf, true}});
        // Still boosted at 20 Hz — a bell would have decayed back to 0 dB.
        REQUIRE_THAT(eq.magnitude_db_at(20.0f), WithinAbs(6.0, 0.6));
        REQUIRE_THAT(eq.magnitude_db_at(18000.0f), WithinAbs(0.0, 0.5));
    }

    SECTION("peak boosts at its center") {
        eq.set_bands({{1000.0f, 6.0f, 2.0f, EqCurveView::FilterType::peak, true}});
        REQUIRE_THAT(eq.magnitude_db_at(1000.0f), WithinAbs(6.0, 0.2));
    }

    SECTION("bands sum, and disabled bands are excluded") {
        eq.set_bands({{1000.0f, 6.0f, 4.0f, EqCurveView::FilterType::peak, true},
                      {1000.0f, 3.0f, 4.0f, EqCurveView::FilterType::peak, true}});
        REQUIRE_THAT(eq.magnitude_db_at(1000.0f), WithinAbs(9.0, 0.3));

        auto bands = eq.bands();
        bands[1].enabled = false;
        eq.set_bands(bands);
        REQUIRE_THAT(eq.magnitude_db_at(1000.0f), WithinAbs(6.0, 0.3));
    }

    SECTION("no bands is a flat 0 dB line") {
        eq.set_bands({});
        REQUIRE_THAT(eq.magnitude_db_at(1000.0f), WithinAbs(0.0, 1e-5));
    }
}

TEST_CASE("EqCurveView response depends on sample rate", "[eq-curve]") {
    // The same 15 kHz low-pass is a mild tilt at 96 kHz but nearly at Nyquist
    // on a 44.1 kHz session. A curve drawn without a sample rate is guesswork.
    EqCurveView a, b;
    const EqCurveView::Band band{15000.0f, 0.0f, 0.707f, EqCurveView::FilterType::low_pass, true};

    a.set_sample_rate(44100.0f);
    a.set_bands({band});
    b.set_sample_rate(96000.0f);
    b.set_bands({band});

    REQUIRE(std::abs(a.magnitude_db_at(19000.0f) - b.magnitude_db_at(19000.0f)) > 1.0f);
}

TEST_CASE("EqCurveView axes are shared and invertible", "[eq-curve][graph_scale]") {
    EqCurveView eq;
    eq.set_bounds({0.0f, 0.0f, 800.0f, 400.0f});
    eq.set_frequency_range(20.0f, 20000.0f);
    eq.set_gain_range(-24.0f, 24.0f);

    auto freq = eq.frequency_scale();
    auto gain = eq.gain_scale();

    // Endpoints land on the edges.
    REQUIRE_THAT(freq.to_x(20.0f), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(freq.to_x(20000.0f), WithinAbs(800.0, 0.01));

    // Logarithmic: 200 Hz and 2 kHz are one decade apart, so they must be the
    // same pixel distance apart as 2 kHz and 20 kHz.
    const float d1 = freq.to_x(2000.0f) - freq.to_x(200.0f);
    const float d2 = freq.to_x(20000.0f) - freq.to_x(2000.0f);
    REQUIRE_THAT(d1, WithinAbs(d2, 0.01));

    // Round trips.
    REQUIRE_THAT(freq.to_frequency(freq.to_x(1000.0f)), WithinAbs(1000.0, 0.1));
    REQUIRE_THAT(gain.to_decibels(gain.to_y(-6.0f)), WithinAbs(-6.0, 0.01));

    // dB grows upward on screen: 0 dB is the vertical center, +24 at the top.
    REQUIRE_THAT(gain.to_y(0.0f), WithinAbs(200.0, 0.01));
    REQUIRE(gain.to_y(12.0f) < gain.to_y(-12.0f));
}

TEST_CASE("spectrum bins resample onto the log axis", "[eq-curve][graph_scale]") {
    // 513 bins = a 1024-point real FFT at 48 kHz: bin k is centered at
    // k * 24000 / 512 Hz. Put a spike in one bin and assert it lands at that
    // bin's TRUE frequency — the old code spread bins linearly across the
    // 20 Hz–20 kHz display range, which put every one of them in the wrong place.
    std::vector<float> bins(513, -100.0f);
    constexpr size_t spike_bin = 128; // 128 * 24000/512 = 6000 Hz
    bins[spike_bin] = 0.0f;

    LogFrequencyScale scale{20.0f, 20000.0f, 0.0f, 1000.0f};
    std::vector<float> out(1000);
    resample_spectrum_log(bins, 48000.0f, scale, out);

    // The peak of the resampled curve sits at the pixel for 6 kHz.
    const auto peak = std::distance(out.begin(), std::max_element(out.begin(), out.end()));
    const float peak_hz = scale.to_frequency(static_cast<float>(peak));
    REQUIRE_THAT(peak_hz, WithinAbs(6000.0, 60.0));

    // Every column has a value — the old bar-per-bin drawing left the bottom
    // decade full of holes because linear bins are sparse there on a log axis.
    REQUIRE(std::none_of(out.begin(), out.end(), [](float v) { return std::isnan(v); }));

    // Above Nyquist there is no data; report the floor rather than a phantom shelf.
    LogFrequencyScale wide{20.0f, 20000.0f, 0.0f, 100.0f};
    std::vector<float> narrow(100);
    resample_spectrum_log(bins, 8000.0f, wide, narrow); // Nyquist = 4 kHz
    REQUIRE(narrow.back() <= pulp::signal::min_response_db);
}

TEST_CASE("EqCurveView shelf response honors Q (view matches audio)", "[eq-curve]") {
    // A shelf's Q shapes its transition. The drawn curve must respond to Q, or
    // it silently disagrees with the audio path (which designs shelves with Q).
    // Regression for the shelf-drops-Q bug: the curve was designed with a fixed
    // slope and ignored Q entirely.
    EqCurveView eq;
    eq.set_sample_rate(48000.0f);

    // Sweep near (not AT) the corner — at the corner |H| is gain/2 dB for any
    // Q, so the resonance a high Q adds only shows to either side.
    const std::vector<float> probes{150.0f, 200.0f, 450.0f, 600.0f, 900.0f};
    eq.set_bands({{300.0f, 8.0f, 0.5f, EqCurveView::FilterType::low_shelf, true}});
    std::vector<float> wide;
    for (float f : probes)
        wide.push_back(eq.magnitude_db_at(f));
    eq.set_bands({{300.0f, 8.0f, 4.0f, EqCurveView::FilterType::low_shelf, true}});
    float max_diff = 0.0f;
    for (size_t i = 0; i < probes.size(); ++i)
        max_diff = std::max(max_diff, std::abs(wide[i] - eq.magnitude_db_at(probes[i])));

    // Changing Q must change the drawn shelf. (If Q were dropped — the bug —
    // every probe would be identical.)
    REQUIRE(max_diff > 1.0f);
}

TEST_CASE("EqCurveView scroll wheel adjusts Q and clamps", "[eq-curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 800, 400});
    eq.set_sample_rate(48000.0f);
    eq.set_bands({{1000.0f, 6.0f, 2.0f, EqCurveView::FilterType::peak, true}});

    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    const float hx = fs.to_x(1000.0f);
    const float hy = gs.to_y(6.0f);

    auto wheel = [&](float delta) {
        MouseEvent e{};
        e.position = {hx, hy};
        e.is_wheel = true;
        e.scroll_delta_y = delta;
        eq.on_mouse_event(e);
    };

    wheel(-1.0f); // scroll up → narrower (higher Q)
    REQUIRE(eq.bands()[0].q > 2.0f);
    const float up_q = eq.bands()[0].q;
    wheel(1.0f); // scroll down → wider
    REQUIRE(eq.bands()[0].q < up_q);

    // Clamps at the bounds no matter how far you scroll.
    for (int i = 0; i < 100; ++i)
        wheel(-1.0f);
    REQUIRE(eq.bands()[0].q <= 12.0f);
    for (int i = 0; i < 100; ++i)
        wheel(1.0f);
    REQUIRE(eq.bands()[0].q >= 0.1f);
}

TEST_CASE("EqCurveView double-click resets gain but keeps the band selected", "[eq-curve]") {
    // Regression: the platform pairs a press with the double-click, and the
    // gain reset moves the handle — the follow-up on_mouse_down must not miss
    // and deselect the band it just reset.
    EqCurveView eq;
    eq.set_bounds({0, 0, 800, 400});
    eq.set_sample_rate(48000.0f);
    eq.set_bands({{1000.0f, 9.0f, 2.0f, EqCurveView::FilterType::peak, true}});

    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    const float hx = fs.to_x(1000.0f);
    const float hy = gs.to_y(9.0f);

    MouseEvent dbl{};
    dbl.position = {hx, hy};
    dbl.is_down = true;
    dbl.click_count = 2;
    eq.on_mouse_event(dbl);
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(0.0, 1e-4));
    REQUIRE(eq.selected_band() == 0);

    // The platform's paired drag-start press (at the now-stale position) must be
    // swallowed, leaving the selection intact rather than clearing it.
    eq.on_mouse_down({hx, hy});
    REQUIRE(eq.selected_band() == 0);
}

TEST_CASE("EqCurveView tracks the hovered band", "[eq-curve]") {
    // Hover arrives via on_hover_move on real hosts, not on_mouse_event.
    EqCurveView eq;
    eq.set_bounds({0, 0, 800, 400});
    eq.set_sample_rate(48000.0f);
    eq.set_bands({{1000.0f, 6.0f, 2.0f, EqCurveView::FilterType::peak, true}});

    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(6.0f)});
    REQUIRE(eq.hovered_band() == 0);

    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(-15.0f)}); // away from the handle
    REQUIRE(eq.hovered_band() == -1);

    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(6.0f)});
    REQUIRE(eq.hovered_band() == 0);
    eq.on_mouse_leave();
    REQUIRE(eq.hovered_band() == -1);
}

TEST_CASE("EqCurveView pinch gesture adjusts the target band Q", "[eq-curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 800, 400});
    eq.set_sample_rate(48000.0f);
    eq.set_bands({{1000.0f, 6.0f, 2.0f, EqCurveView::FilterType::peak, true}});

    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(6.0f)}); // hover the handle
    REQUIRE(eq.hovered_band() == 0);

    // began / ended are boundaries — no scale delta is applied.
    GestureEvent begin{};
    begin.phase = GesturePhase::began;
    begin.delta_scale = -0.2f;
    eq.on_gesture_event(begin);
    REQUIRE_THAT(eq.bands()[0].q, WithinAbs(2.0, 1e-4));

    // pinch-IN (negative magnification) narrows → higher Q.
    GestureEvent pinch_in{};
    pinch_in.phase = GesturePhase::changed;
    pinch_in.delta_scale = -0.2f;
    eq.on_gesture_event(pinch_in);
    REQUIRE(eq.bands()[0].q > 2.0f);

    // pinch-OUT (positive) widens → lower Q, clamped at the 0.1 floor no matter
    // how far it is pushed.
    for (int i = 0; i < 60; ++i) {
        GestureEvent pinch_out{};
        pinch_out.phase = GesturePhase::changed;
        pinch_out.delta_scale = 0.3f;
        eq.on_gesture_event(pinch_out);
    }
    REQUIRE(eq.bands()[0].q >= 0.1f);
    REQUIRE(eq.bands()[0].q < 2.0f);
}

TEST_CASE("EqCurveView pinch with no target band is a safe no-op", "[eq-curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 800, 400});
    // Nothing hovered or selected, no bands: the gesture returns without
    // indexing an empty band vector.
    GestureEvent pinch{};
    pinch.phase = GesturePhase::changed;
    pinch.delta_scale = -0.3f;
    eq.on_gesture_event(pinch);
    REQUIRE(eq.band_count() == 0);
}

TEST_CASE("EqCurveView dragging a shelf moves gain at twice the handle rate", "[eq-curve]") {
    // A shelf handle rides its curve at gain/2, so to keep the dot under the
    // pointer the stored gain must move at twice the handle's dB.
    EqCurveView eq;
    eq.set_bounds({0, 0, 800, 400});
    eq.set_sample_rate(48000.0f);
    eq.set_gain_range(-24.0f, 24.0f);
    eq.set_bands({{300.0f, 0.0f, 1.0f, EqCurveView::FilterType::low_shelf, true}});

    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    const float hx = fs.to_x(300.0f);
    eq.on_mouse_down({hx, gs.to_y(0.0f)}); // grab the flat shelf's handle (0 dB line)
    REQUIRE(eq.selected_band() == 0);

    // Drag to where +6 dB plots; the shelf gain lands near +12 dB.
    eq.on_mouse_drag({hx, gs.to_y(6.0f)});
    REQUIRE(eq.bands()[0].gain_db > 9.0f);
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(12.0, 2.0));
}

TEST_CASE("EqCurveView paint covers readout, per-band fills, and disabled handles", "[eq-curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 480, 240});
    eq.set_sample_rate(48000.0f);
    eq.set_gain_range(-24.0f, 24.0f);

    // Flip each display flag through its setter and read it back (also exercises
    // the header inline accessors that the diff added).
    eq.set_show_band_curves(true);
    REQUIRE(eq.show_band_curves());
    eq.set_show_labels(true);
    REQUIRE(eq.show_labels());
    eq.set_show_readout(true);
    REQUIRE(eq.show_readout());
    eq.set_show_disabled_handles(true);
    REQUIRE(eq.show_disabled_handles());

    // A peaking band (gain readout + sub-kHz label), a shelf, a gain-less
    // low-pass (0 dB handle), and a bypassed band drawn dimmed.
    eq.set_bands({
        {440.0f, 8.0f, 2.0f, EqCurveView::FilterType::peak, true},
        {180.0f, 6.0f, 0.7f, EqCurveView::FilterType::low_shelf, true},
        {8000.0f, 0.0f, 0.7f, EqCurveView::FilterType::low_pass, true},
        {2000.0f, -6.0f, 3.0f, EqCurveView::FilterType::peak, false}, // bypassed
    });

    // Two set_spectrum calls at the same bin count exercise the temporal
    // smoothing loop (first assigns, second eases each bin — attack path since
    // it gets louder).
    std::vector<float> mags(64, 0.0f);
    for (size_t i = 0; i < mags.size(); ++i)
        mags[i] = -30.0f + static_cast<float>(i);
    eq.set_spectrum(mags.data(), mags.size());
    for (auto& m : mags)
        m += 6.0f;
    eq.set_spectrum(mags.data(), mags.size());

    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();

    auto any_text_contains = [](const RecordingCanvas& c, std::string_view needle) {
        return std::any_of(c.commands().begin(), c.commands().end(), [&](const DrawCommand& cmd) {
            return cmd.type == DrawCommand::Type::fill_text &&
                   cmd.text.find(needle) != std::string::npos;
        });
    };

    // Hover the peaking band → readout draws its gain field and a sub-kHz "Hz"
    // frequency; all four handles paint (the bypassed one dimmed).
    eq.on_hover_move({fs.to_x(440.0f), gs.to_y(8.0f)});
    REQUIRE(eq.hovered_band() == 0);
    RecordingCanvas c1;
    eq.paint(c1);
    REQUIRE(c1.count(DrawCommand::Type::fill_circle) == 4);
    REQUIRE(c1.count(DrawCommand::Type::set_fill_gradient_linear) >= 1);
    REQUIRE(any_text_contains(c1, "dB")); // gain readout field
    REQUIRE(any_text_contains(c1, "Hz")); // sub-kHz frequency field

    // Hover the gain-less low-pass → the readout reserves the gain cell (no dB
    // value) and formats a kHz frequency.
    eq.on_hover_move({fs.to_x(8000.0f), gs.to_y(0.0f)});
    REQUIRE(eq.hovered_band() == 2);
    RecordingCanvas c2;
    eq.paint(c2);
    REQUIRE(any_text_contains(c2, "kHz"));
}

TEST_CASE("EqCurveView hover animation eases the handle radius then settles", "[eq-curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 480, 240});
    eq.set_sample_rate(48000.0f);
    eq.set_bands({{1000.0f, 6.0f, 2.0f, EqCurveView::FilterType::peak, true}});
    eq.set_hover_animation(true);
    REQUIRE(eq.hover_animation());

    // First frame snaps the radius to its resting target — not animating yet.
    RecordingCanvas f0;
    eq.paint(f0);
    REQUIRE_FALSE(eq.hover_animating());

    // Hovering raises the target radius; the next frame is mid-ease.
    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(6.0f)});
    RecordingCanvas f1;
    eq.paint(f1);
    REQUIRE(eq.hover_animating());

    // It settles within a bounded number of frames and the flag clears.
    bool settled = false;
    for (int i = 0; i < 60 && !settled; ++i) {
        RecordingCanvas f;
        eq.paint(f);
        settled = !eq.hover_animating();
    }
    REQUIRE(settled);
}

TEST_CASE("EqCurveView content top padding shifts the plot and clamps handles", "[eq-curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 480, 240});
    eq.set_sample_rate(48000.0f);
    eq.set_gain_range(-24.0f, 24.0f);

    eq.set_content_top_padding(40.0f);
    REQUIRE_THAT(eq.content_top_padding(), WithinAbs(40.0, 1e-4));
    eq.set_content_top_padding(-5.0f); // negatives floor to 0
    REQUIRE_THAT(eq.content_top_padding(), WithinAbs(0.0, 1e-4));
    eq.set_content_top_padding(40.0f);

    // A band pinned at max gain would plot above the reserved top; its handle
    // clamps into the plot and stays grabbable via the exposed hit_test_handle.
    eq.set_bands({{1000.0f, 24.0f, 2.0f, EqCurveView::FilterType::peak, true}});
    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale(); // padded scale: +24 dB maps to the padded top
    REQUIRE(eq.hit_test_handle({fs.to_x(1000.0f), gs.to_y(24.0f)}) == 0);

    RecordingCanvas c;
    eq.paint(c);
    REQUIRE(c.count(DrawCommand::Type::fill_circle) == 1);
}

TEST_CASE("EqCurveView exposes a distinct wrapping band palette", "[eq-curve]") {
    const Color c0 = EqCurveView::band_palette_color(0);
    const Color c1 = EqCurveView::band_palette_color(1);
    REQUIRE_FALSE(c0 == c1);                           // adjacent bands differ
    REQUIRE(EqCurveView::band_palette_color(8) == c0); // wraps modulo the palette
    REQUIRE(c0.a > 0.0f);                              // opaque — usable as a fill
}

TEST_CASE("EqCurveView clear_spectrum removes the analyzer overlay", "[eq-curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 240, 120});
    std::vector<float> mags(32, -12.0f);
    eq.set_spectrum(mags.data(), mags.size());

    RecordingCanvas with_spec;
    eq.paint(with_spec);
    const int with = with_spec.count(DrawCommand::Type::set_fill_gradient_linear);
    REQUIRE(with >= 1); // the analyzer draws a gradient envelope

    eq.clear_spectrum();
    RecordingCanvas without_spec;
    eq.paint(without_spec);
    // No bands and no spectrum → the gradient envelope is gone.
    REQUIRE(without_spec.count(DrawCommand::Type::set_fill_gradient_linear) < with);
}

TEST_CASE("EqCurveView band management", "[eq-curve]") {
    EqCurveView eq;

    SECTION("Add and remove bands") {
        eq.add_band({1000.0f, 3.0f, 1.0f, EqCurveView::FilterType::peak, true});
        eq.add_band({200.0f, -6.0f, 0.7f, EqCurveView::FilterType::low_shelf, true});
        REQUIRE(eq.band_count() == 2);

        eq.remove_band(0);
        REQUIRE(eq.band_count() == 1);
        REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(200.0, 0.1));
    }

    SECTION("Set bands replaces all") {
        std::vector<EqCurveView::Band> bands = {
            {100.0f, 0.0f, 1.0f, EqCurveView::FilterType::high_pass, true},
            {5000.0f, -3.0f, 2.0f, EqCurveView::FilterType::peak, true}};
        eq.set_bands(bands);
        REQUIRE(eq.band_count() == 2);
    }

    SECTION("Frequency and gain ranges") {
        eq.set_frequency_range(10.0f, 24000.0f);
        REQUIRE_THAT(eq.min_frequency(), WithinAbs(10.0, 0.1));
        REQUIRE_THAT(eq.max_frequency(), WithinAbs(24000.0, 0.1));

        eq.set_gain_range(-30.0f, 30.0f);
        REQUIRE_THAT(eq.min_gain(), WithinAbs(-30.0, 0.1));
        REQUIRE_THAT(eq.max_gain(), WithinAbs(30.0, 0.1));
    }

    SECTION("Selected band") {
        eq.add_band({1000.0f, 0.0f, 1.0f});
        eq.set_selected_band(0);
        REQUIRE(eq.selected_band() == 0);
    }

    SECTION("Invalid band operations are no-ops and range setters clamp") {
        eq.add_band({500.0f, -3.0f, 0.8f, EqCurveView::FilterType::peak, true});

        EqCurveView::Band replacement{2000.0f, 6.0f, 2.0f, EqCurveView::FilterType::notch, true};
        eq.set_band(3, replacement);
        eq.remove_band(9);
        REQUIRE(eq.band_count() == 1);
        REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(500.0, 0.1));
        REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(-3.0, 0.1));
        REQUIRE_THAT(eq.bands()[0].q, WithinAbs(0.8, 0.01));

        eq.set_frequency_range(-20.0f, -10.0f);
        REQUIRE_THAT(eq.min_frequency(), WithinAbs(1.0, 0.001));
        REQUIRE_THAT(eq.max_frequency(), WithinAbs(2.0, 0.001));

        eq.set_gain_range(12.0f, 12.0f);
        REQUIRE_THAT(eq.min_gain(), WithinAbs(12.0, 0.001));
        REQUIRE_THAT(eq.max_gain(), WithinAbs(13.0, 0.001));
    }
}

TEST_CASE("EqCurveView spectrum overlay", "[eq-curve]") {
    EqCurveView eq;
    std::vector<float> spectrum(128, -60.0f);
    eq.set_spectrum(spectrum.data(), spectrum.size());
    eq.clear_spectrum();
    // Should not crash
}

TEST_CASE("EqCurveView hit testing and drag callbacks", "[eq-curve][issue-493]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 200, 100});
    eq.set_frequency_range(100.0f, 10000.0f);
    eq.set_gain_range(-12.0f, 12.0f);
    eq.add_band({1000.0f, 0.0f, 1.0f});

    int selected_index = -1;
    int selected_count = 0;
    int changed_index = -1;
    int changed_count = 0;
    EqCurveView::Band changed_band{};
    eq.on_band_selected = [&](size_t index) {
        selected_index = static_cast<int>(index);
        ++selected_count;
    };
    eq.on_band_changed = [&](size_t index, EqCurveView::Band band) {
        changed_index = static_cast<int>(index);
        changed_band = band;
        ++changed_count;
    };

    eq.on_mouse_down({100, 50});
    REQUIRE(eq.selected_band() == 0);
    REQUIRE(selected_index == 0);
    REQUIRE(selected_count == 1);

    eq.on_mouse_drag({400, -100});
    REQUIRE(changed_index == 0);
    REQUIRE(changed_count == 1);
    REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(10000.0, 0.1));
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(12.0, 0.001));
    REQUIRE_THAT(changed_band.frequency, WithinAbs(10000.0, 0.1));
    REQUIRE_THAT(changed_band.gain_db, WithinAbs(12.0, 0.001));

    eq.on_mouse_up({400, -100});
    eq.on_mouse_drag({0, 100});
    REQUIRE(changed_count == 1);
}

TEST_CASE("EqCurveView empty hit does not start a drag", "[eq-curve][issue-493]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 200, 100});
    eq.set_frequency_range(100.0f, 10000.0f);
    eq.set_gain_range(-12.0f, 12.0f);
    eq.add_band({1000.0f, 0.0f, 1.0f});

    int selected_count = 0;
    int changed_count = 0;
    eq.on_band_selected = [&](size_t) { ++selected_count; };
    eq.on_band_changed = [&](size_t, EqCurveView::Band) { ++changed_count; };

    eq.on_mouse_down({5, 5});
    REQUIRE(eq.selected_band() == -1);
    REQUIRE(selected_count == 0);

    eq.on_mouse_drag({200, 0});
    REQUIRE(changed_count == 0);
    REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(1000.0, 0.1));
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(0.0, 0.001));
}

TEST_CASE("EqCurveView paint covers disabled grid and disabled band handles",
          "[eq-curve][issue-652]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 240, 120});
    eq.set_show_grid(false);
    eq.add_band({500.0f, 12.0f, 1.0f, EqCurveView::FilterType::peak, false});

    ::vellum::canvas::RecordingCanvas canvas;
    eq.paint(canvas);

    REQUIRE(canvas.count(::vellum::canvas::DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(::vellum::canvas::DrawCommand::Type::fill_circle) == 0);
    REQUIRE(canvas.count(::vellum::canvas::DrawCommand::Type::stroke_circle) == 0);
    REQUIRE(canvas.count(::vellum::canvas::DrawCommand::Type::stroke_line) > 0);
}

TEST_CASE("EqCurveView set_band updates valid indices", "[eq-curve]") {
    EqCurveView eq;
    eq.add_band({500.0f, -3.0f, 0.8f, EqCurveView::FilterType::peak, true});

    EqCurveView::Band replacement{2000.0f, 4.0f, 1.5f, EqCurveView::FilterType::notch, true};
    eq.set_band(0, replacement);

    REQUIRE(eq.band_count() == 1);
    REQUIRE_THAT(eq.bands()[0].frequency, WithinAbs(2000.0f, 0.001f));
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(4.0f, 0.001f));
    REQUIRE_THAT(eq.bands()[0].q, WithinAbs(1.5f, 0.001f));
    REQUIRE(eq.bands()[0].type == EqCurveView::FilterType::notch);
}

TEST_CASE("EqCurveView paint covers grid spectrum and enabled handles", "[eq-curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 240, 120});
    const float spectrum[] = {-18.0f, -12.0f, -24.0f, -6.0f};
    eq.set_spectrum(spectrum, sizeof(spectrum) / sizeof(spectrum[0]));
    eq.add_band(
        {1000.0f, 6.0f, 1.0f, EqCurveView::FilterType::peak, true, Color::rgba8(240, 20, 20)});
    eq.set_selected_band(0);

    RecordingCanvas canvas;
    eq.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 1); // background
    // The spectrum is now a smooth gradient-filled envelope (not per-column
    // bars): its soft vertical wash plus the band's own fill both go through
    // set_fill_gradient_linear, so at least one gradient fill is recorded.
    REQUIRE(canvas.count(DrawCommand::Type::set_fill_gradient_linear) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) > 200);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_circle) == 1);
    REQUIRE(has_fill_color(canvas, Color::rgba8(240, 20, 20)));
}

TEST_CASE("EqCurveView forwards rich mouse events to the base pointer hook", "[eq-curve]") {
    EqCurveView eq;
    int forwarded = 0;
    eq.on_pointer_event = [&](const MouseEvent& event) {
        ++forwarded;
        REQUIRE(event.pointer_id == 9);
    };

    eq.on_mouse_event(MouseEvent{{8, 9}, {8, 9}, MouseButton::left, 0, 9, 1, true});

    REQUIRE(forwarded == 1);
}

TEST_CASE("EqCurveView analyzer overlay maps bins through SpectrumBinMapping", "[eq-curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 640, 240});
    eq.set_sample_rate(48000.0f);

    // A narrow peak between two bins of a 2048-point spectrum.
    std::vector<float> bins(1025, -90.0f);
    const double vertex = 1000.0 / (48000.0 / 2048.0);
    for (int k = 0; k < 1025; ++k)
        bins[static_cast<std::size_t>(k)] =
            std::max(-90.0f, -3.0f - 6.0f * static_cast<float>((k - vertex) * (k - vertex)));
    eq.set_spectrum(bins.data(), bins.size());

    RecordingCanvas with_overlay;
    eq.paint(with_overlay);
    CHECK(eq.analyzer_animating());
    eq.set_analyzer_enabled(false);
    RecordingCanvas without_overlay;
    eq.paint(without_overlay);
    eq.set_analyzer_enabled(true);
    // The overlay's top stroke is a polyline of many segments.
    CHECK(with_overlay.count(DrawCommand::Type::stroke_line) >
          without_overlay.count(DrawCommand::Type::stroke_line) + 100);

    // A bin count that changes between frames (FFT-size change) repaints cleanly.
    std::vector<float> bigger(4097, -60.0f);
    eq.set_spectrum(bigger.data(), bigger.size());
    RecordingCanvas again;
    eq.paint(again);
    CHECK(again.count(DrawCommand::Type::stroke_line) >
          without_overlay.count(DrawCommand::Type::stroke_line) + 100);

    // A degenerate one-bin spectrum draws a floor line, not garbage.
    float single = -20.0f;
    eq.set_spectrum(&single, 1);
    RecordingCanvas degenerate;
    eq.paint(degenerate);
}

TEST_CASE("an EqCurveView mid hover-settle needs continuous frames",
          "[eq-curve][continuous-frames]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 320, 160});
    eq.set_sample_rate(48000.0f);
    eq.set_bands({{1000.0f, 6.0f, 2.0f, EqCurveView::FilterType::peak, true}});
    eq.set_hover_animation(true);

    ::vellum::canvas::RecordingCanvas snap;
    eq.paint(snap); // first frame snaps — settled
    REQUIRE_FALSE(eq.hover_animating());
    REQUIRE_FALSE(needs_continuous_frames(&eq));

    // Hovering raises the handle's target radius; the next frame is mid-ease, so
    // the shared predicate must keep the render loop alive.
    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(6.0f)});
    ::vellum::canvas::RecordingCanvas easing;
    eq.paint(easing);
    REQUIRE(eq.hover_animating());
    REQUIRE(needs_continuous_frames(&eq));

    // Once it settles the flag clears and the loop is allowed to idle again.
    for (int i = 0; i < 60 && eq.hover_animating(); ++i) {
        ::vellum::canvas::RecordingCanvas f;
        eq.paint(f);
    }
    REQUIRE_FALSE(eq.hover_animating());
    REQUIRE_FALSE(needs_continuous_frames(&eq));
}

TEST_CASE("continuous-frame registration is scoped to the owning kind",
          "[eq-curve][continuous-frames]") {
    EqCurveView eq;
    ::vellum::view::View unrelated;

    REQUIRE_FALSE(needs_continuous_frames(&unrelated));
    REQUIRE_FALSE(needs_continuous_frames(&eq));

    eq.set_analyzer_enabled(true);
    const float spectrum[] = {-12.0f, -18.0f, -24.0f};
    eq.set_spectrum(spectrum, 3);
    CHECK(needs_continuous_frames(&eq));
    CHECK_FALSE(needs_continuous_frames(&unrelated));
}

TEST_CASE("EqCurveView tags itself as the eq_curve runtime kind", "[eq-curve][continuous-frames]") {
    EqCurveView eq;
    ::vellum::view::View generic;
    CHECK(eq.runtime_view_kind() == ::vellum::view::RuntimeViewKind::eq_curve);
    CHECK(generic.runtime_view_kind() == ::vellum::view::RuntimeViewKind::generic);
}

namespace {
struct EqRaster {
    std::vector<std::uint8_t> px;
    std::uint32_t w = 0, h = 0;
    int at(int x, int y, int c) const {
        return px[(static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4 + c];
    }
    // First row in column x whose pixel reads as the accent-blue curve.
    int curve_row(int x) const {
        for (int y = 0; y < static_cast<int>(h); ++y)
            if (at(x, y, 2) > 170 && at(x, y, 1) > 110 && at(x, y, 2) - at(x, y, 0) > 60)
                return y;
        return -1;
    }
};
EqRaster eq_raster(EqCurveView& eq, std::uint32_t w, std::uint32_t h) {
    EqRaster r;
    r.px = ::vellum::view::render_to_rgba(eq, w, h, 1.0f, &r.w, &r.h);
    return r;
}
} // namespace

TEST_CASE("EqCurveView raster draws the true response of each filter type", "[eq-curve][render]") {
    if (!::vellum::view::raw_rgba_render_available())
        SKIP("Vellum CPU raster backend unavailable");

    auto make = [](EqCurveView::FilterType type, float gain_db) {
        auto eq = std::make_unique<EqCurveView>();
        eq->set_sample_rate(48000.0f);
        eq->set_show_grid(false);
        eq->set_show_labels(false);
        eq->set_show_readout(false);
        eq->set_show_band_curves(false);
        eq->set_analyzer_enabled(false);
        eq->set_bounds({0, 0, 900, 400});
        eq->set_bands({{1000.0f, gain_db, 0.9f, type, true}});
        return eq;
    };
    const float zero_y = [&] {
        auto eq = make(EqCurveView::FilterType::peak, 0.0f);
        return eq->gain_scale().to_y(0.0f);
    }();

    // A +12 dB peak lifts the curve above the 0 dB line at 1 kHz.
    auto peak = make(EqCurveView::FilterType::peak, 12.0f);
    const auto praster = eq_raster(*peak, 900, 400);
    const auto fs = peak->frequency_scale();
    const int at_1k = praster.curve_row(static_cast<int>(fs.to_x(1500.0f)));
    REQUIRE(at_1k >= 0);
    CHECK(at_1k < zero_y - 20.0f);

    // A low-pass is the opposite shape, not a bell: unpeaked at its corner and
    // rolled well below the 0 dB line above it.
    auto lp = make(EqCurveView::FilterType::low_pass, 0.0f);
    const auto lraster = eq_raster(*lp, 900, 400);
    if (const char* dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR")) {
        CHECK(::vellum::view::render_to_file(*peak, 900, 400, std::string(dir) + "/eq-peak.png",
                                             1.0f, ::vellum::view::ScreenshotBackend::skia));
        CHECK(::vellum::view::render_to_file(*lp, 900, 400, std::string(dir) + "/eq-lowpass.png",
                                             1.0f, ::vellum::view::ScreenshotBackend::skia));
    }
    const int lp_mid = lraster.curve_row(static_cast<int>(fs.to_x(600.0f)));
    const int lp_high = lraster.curve_row(static_cast<int>(fs.to_x(2500.0f)));
    REQUIRE(lp_mid >= 0);
    REQUIRE(lp_high >= 0);
    CHECK(lp_mid > zero_y - 10.0f); // no peak at the corner
    CHECK(lp_high > zero_y + 60.0f);
}

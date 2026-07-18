#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/eq_curve_view.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/color_picker.hpp>
#include <pulp/view/file_drop_zone.hpp>
#include <pulp/view/split_view.hpp>
#include <pulp/view/property_list.hpp>
#include <pulp/view/breadcrumb.hpp>
#include <pulp/view/theme_editor.hpp>
#include <pulp/view/graph_scale.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/state/store.hpp>
#include <pulp/signal/frequency_response.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using Catch::Matchers::WithinAbs;

namespace {

bool has_text(const RecordingCanvas& canvas, std::string_view text) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const DrawCommand& command) {
                           return command.type == DrawCommand::Type::fill_text &&
                                  command.text == text;
                       });
}

bool has_fill_color(const RecordingCanvas& canvas, Color color) {
    return std::any_of(canvas.commands().begin(),
                       canvas.commands().end(),
                       [&](const DrawCommand& command) {
                           return command.type == DrawCommand::Type::set_fill_color &&
                                  command.color == color;
                       });
}

} // namespace

// ── EqCurveView: StateStore binding (parameter automation) ──────────────────
//
// bind_bands() wires the widget to a StateStore so drags RECORD host automation
// (begin_gesture → set_value → end_gesture) and host writes PLAY BACK onto the
// curve (a Main-thread listener drained by pump_listeners). These assert the two
// directions and the no-feedback guard.

TEST_CASE("EqCurveView bind_bands records a dot drag as one gesture",
          "[view][eq_curve][automation]") {
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
    auto rec = store.add_listener(
        [&](ParamID id, float v) { sets.emplace_back(id, v); }, ListenerThread::Audio);

    EqCurveView eq;
    eq.set_sample_rate(48000.0f);
    eq.set_frequency_range(20.0f, 20000.0f);
    eq.set_gain_range(-24.0f, 24.0f);
    eq.set_bounds({0, 0, 400, 300});
    // Low-pass: a dot drag moves ONLY frequency (no gain), so exactly one param
    // gesture opens and closes.
    eq.set_bands({{1000.0f, 0.0f, 0.707f, EqCurveView::FilterType::low_pass, true}});
    std::array<EqCurveView::BandParamIds, 1> ids{{{1, 2, 3}}};
    eq.bind_bands(store,
                  std::span<const EqCurveView::BandParamIds>(ids.data(), ids.size()));

    const float hy = eq.gain_scale().to_y(0.0f);  // low-pass handle rides 0 dB
    eq.on_mouse_down({eq.frequency_scale().to_x(1000.0f), hy});
    eq.on_mouse_drag({eq.frequency_scale().to_x(4000.0f), hy});
    eq.on_mouse_up({eq.frequency_scale().to_x(4000.0f), hy});

    REQUIRE(begins == 1);  // exactly one begin_gesture (frequency only)
    REQUIRE(ends == 1);    // and exactly one end_gesture
    // At least one frequency set_value was emitted during the drag.
    REQUIRE(std::any_of(sets.begin(), sets.end(),
                        [](const auto& s) { return s.first == 1u; }));
    // A gain-less filter writes neither gain nor Q.
    REQUIRE_FALSE(std::any_of(sets.begin(), sets.end(), [](const auto& s) {
        return s.first == 2u || s.first == 3u;
    }));
    // The recorded band actually moved.
    REQUIRE(eq.bands()[0].frequency > 1000.0f);
}

TEST_CASE("EqCurveView bind_bands records scroll-Q and reset as gestures",
          "[view][eq_curve][automation]") {
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
    auto rec = store.add_listener(
        [&](ParamID id, float v) { sets.emplace_back(id, v); }, ListenerThread::Audio);

    EqCurveView eq;
    eq.set_sample_rate(48000.0f);
    eq.set_frequency_range(20.0f, 20000.0f);
    eq.set_gain_range(-24.0f, 24.0f);
    eq.set_bounds({0, 0, 400, 300});
    eq.set_bands({{1000.0f, 6.0f, 1.0f, EqCurveView::FilterType::peak, true}});
    std::array<EqCurveView::BandParamIds, 1> ids{{{1, 2, 3}}};
    eq.bind_bands(store,
                  std::span<const EqCurveView::BandParamIds>(ids.data(), ids.size()));

    const float hx = eq.frequency_scale().to_x(1000.0f);
    const float hy = eq.gain_scale().to_y(6.0f);  // peak handle rides its gain

    SECTION("scroll wheel over a dot records a Q gesture") {
        MouseEvent e;
        e.is_wheel = true;
        e.position = {hx, hy};
        e.scroll_delta_y = -6.0f;  // narrow → raise Q
        eq.on_mouse_event(e);

        REQUIRE(begins == 1);
        REQUIRE(ends == 1);
        REQUIRE(std::any_of(sets.begin(), sets.end(),
                            [](const auto& s) { return s.first == 3u; }));  // Q written
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
          "[view][eq_curve][automation]") {
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
    eq.bind_bands(store,
                  std::span<const EqCurveView::BandParamIds>(ids.data(), ids.size()));

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

// ── EqCurveView: analyzer full-height dBFS scale ─────────────────────────────
//
// The spectrum overlay maps onto a dedicated dBFS scale spanning the FULL plot
// height (0 dBFS at the top → −60 dBFS at the bottom), decoupled from the ±gain
// axis so a broadband envelope fills the whole graph (Logic / Pro-Q style).

TEST_CASE("EqCurveView analyzer scale spans full plot height",
          "[view][eq_curve][analyzer]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 400, 300});  // plot: y 0 (top) .. 300 (bottom)

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

// Headless PNG proof that the analyzer envelope spans the full height. Opt-in
// (hidden [.render] tag + env guard) so CI stays file-free; run with
//   PULP_EQ_RENDER_PNG=1 ctest -R phase9-widgets
// or the binary directly with the tag, to write /tmp/eq-analyzer-fullheight.png.
TEST_CASE("EqCurveView analyzer renders full-height (headless PNG)",
          "[view][eq_curve][analyzer][.render]") {
    if (std::getenv("PULP_EQ_RENDER_PNG") == nullptr) return;

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
    for (std::size_t i = 8; i < 48; ++i) mags[i] = -18.0f;
    eq.set_spectrum(mags.data(), mags.size());

    const bool ok = render_to_file(eq, 900, 400, "/tmp/eq-analyzer-fullheight.png", 2.0f);
    REQUIRE(ok);
}

// ── EqCurveView: the drawn curve ────────────────────────────────────────────
//
// The band-management tests below never look at the curve, which is how this
// widget shipped for months drawing a Gaussian bell for EVERY band type — a
// low-pass rendered as a symmetric bump. These assert the shape.

TEST_CASE("EqCurveView draws the true response, not a bell", "[view][eq_curve]") {
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

TEST_CASE("EqCurveView response depends on sample rate", "[view][eq_curve]") {
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

TEST_CASE("EqCurveView axes are shared and invertible", "[view][eq_curve][graph_scale]") {
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

TEST_CASE("spectrum bins resample onto the log axis", "[view][eq_curve][graph_scale]") {
    // 513 bins = a 1024-point real FFT at 48 kHz: bin k is centered at
    // k * 24000 / 512 Hz. Put a spike in one bin and assert it lands at that
    // bin's TRUE frequency — the old code spread bins linearly across the
    // 20 Hz–20 kHz display range, which put every one of them in the wrong place.
    std::vector<float> bins(513, -100.0f);
    constexpr size_t spike_bin = 128;             // 128 * 24000/512 = 6000 Hz
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

TEST_CASE("EqCurveView shelf response honors Q (view matches audio)",
          "[view][eq_curve]") {
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
    for (float f : probes) wide.push_back(eq.magnitude_db_at(f));
    eq.set_bands({{300.0f, 8.0f, 4.0f, EqCurveView::FilterType::low_shelf, true}});
    float max_diff = 0.0f;
    for (size_t i = 0; i < probes.size(); ++i)
        max_diff = std::max(max_diff, std::abs(wide[i] - eq.magnitude_db_at(probes[i])));

    // Changing Q must change the drawn shelf. (If Q were dropped — the bug —
    // every probe would be identical.)
    REQUIRE(max_diff > 1.0f);
}

TEST_CASE("EqCurveView scroll wheel adjusts Q and clamps", "[view][eq_curve]") {
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

    wheel(-1.0f);  // scroll up → narrower (higher Q)
    REQUIRE(eq.bands()[0].q > 2.0f);
    const float up_q = eq.bands()[0].q;
    wheel(1.0f);   // scroll down → wider
    REQUIRE(eq.bands()[0].q < up_q);

    // Clamps at the bounds no matter how far you scroll.
    for (int i = 0; i < 100; ++i) wheel(-1.0f);
    REQUIRE(eq.bands()[0].q <= 12.0f);
    for (int i = 0; i < 100; ++i) wheel(1.0f);
    REQUIRE(eq.bands()[0].q >= 0.1f);
}

TEST_CASE("EqCurveView double-click resets gain but keeps the band selected",
          "[view][eq_curve]") {
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

TEST_CASE("EqCurveView tracks the hovered band", "[view][eq_curve]") {
    // Hover arrives via on_hover_move on real hosts, not on_mouse_event.
    EqCurveView eq;
    eq.set_bounds({0, 0, 800, 400});
    eq.set_sample_rate(48000.0f);
    eq.set_bands({{1000.0f, 6.0f, 2.0f, EqCurveView::FilterType::peak, true}});

    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(6.0f)});
    REQUIRE(eq.hovered_band() == 0);

    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(-15.0f)});  // away from the handle
    REQUIRE(eq.hovered_band() == -1);

    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(6.0f)});
    REQUIRE(eq.hovered_band() == 0);
    eq.on_mouse_leave();
    REQUIRE(eq.hovered_band() == -1);
}

TEST_CASE("EqCurveView pinch gesture adjusts the target band Q", "[view][eq_curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 800, 400});
    eq.set_sample_rate(48000.0f);
    eq.set_bands({{1000.0f, 6.0f, 2.0f, EqCurveView::FilterType::peak, true}});

    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();
    eq.on_hover_move({fs.to_x(1000.0f), gs.to_y(6.0f)});  // hover the handle
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

TEST_CASE("EqCurveView pinch with no target band is a safe no-op", "[view][eq_curve]") {
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

TEST_CASE("EqCurveView dragging a shelf moves gain at twice the handle rate",
          "[view][eq_curve]") {
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
    eq.on_mouse_down({hx, gs.to_y(0.0f)});   // grab the flat shelf's handle (0 dB line)
    REQUIRE(eq.selected_band() == 0);

    // Drag to where +6 dB plots; the shelf gain lands near +12 dB.
    eq.on_mouse_drag({hx, gs.to_y(6.0f)});
    REQUIRE(eq.bands()[0].gain_db > 9.0f);
    REQUIRE_THAT(eq.bands()[0].gain_db, WithinAbs(12.0, 2.0));
}

TEST_CASE("EqCurveView paint covers readout, per-band fills, and disabled handles",
          "[view][eq_curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 480, 240});
    eq.set_sample_rate(48000.0f);
    eq.set_gain_range(-24.0f, 24.0f);

    // Flip each display flag through its setter and read it back (also exercises
    // the header inline accessors that the diff added).
    eq.set_show_band_curves(true);      REQUIRE(eq.show_band_curves());
    eq.set_show_labels(true);           REQUIRE(eq.show_labels());
    eq.set_show_readout(true);          REQUIRE(eq.show_readout());
    eq.set_show_disabled_handles(true); REQUIRE(eq.show_disabled_handles());

    // A peaking band (gain readout + sub-kHz label), a shelf, a gain-less
    // low-pass (0 dB handle), and a bypassed band drawn dimmed.
    eq.set_bands({
        {440.0f,   8.0f, 2.0f, EqCurveView::FilterType::peak,      true},
        {180.0f,   6.0f, 0.7f, EqCurveView::FilterType::low_shelf, true},
        {8000.0f,  0.0f, 0.7f, EqCurveView::FilterType::low_pass,  true},
        {2000.0f, -6.0f, 3.0f, EqCurveView::FilterType::peak,      false},  // bypassed
    });

    // Two set_spectrum calls at the same bin count exercise the temporal
    // smoothing loop (first assigns, second eases each bin — attack path since
    // it gets louder).
    std::vector<float> mags(64, 0.0f);
    for (size_t i = 0; i < mags.size(); ++i)
        mags[i] = -30.0f + static_cast<float>(i);
    eq.set_spectrum(mags.data(), mags.size());
    for (auto& m : mags) m += 6.0f;
    eq.set_spectrum(mags.data(), mags.size());

    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();

    auto any_text_contains = [](const RecordingCanvas& c, std::string_view needle) {
        return std::any_of(c.commands().begin(), c.commands().end(),
                           [&](const DrawCommand& cmd) {
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
    REQUIRE(any_text_contains(c1, "dB"));   // gain readout field
    REQUIRE(any_text_contains(c1, "Hz"));   // sub-kHz frequency field

    // Hover the gain-less low-pass → the readout reserves the gain cell (no dB
    // value) and formats a kHz frequency.
    eq.on_hover_move({fs.to_x(8000.0f), gs.to_y(0.0f)});
    REQUIRE(eq.hovered_band() == 2);
    RecordingCanvas c2;
    eq.paint(c2);
    REQUIRE(any_text_contains(c2, "kHz"));
}

TEST_CASE("EqCurveView hover animation eases the handle radius then settles",
          "[view][eq_curve]") {
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

TEST_CASE("EqCurveView content top padding shifts the plot and clamps handles",
          "[view][eq_curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 480, 240});
    eq.set_sample_rate(48000.0f);
    eq.set_gain_range(-24.0f, 24.0f);

    eq.set_content_top_padding(40.0f);
    REQUIRE_THAT(eq.content_top_padding(), WithinAbs(40.0, 1e-4));
    eq.set_content_top_padding(-5.0f);   // negatives floor to 0
    REQUIRE_THAT(eq.content_top_padding(), WithinAbs(0.0, 1e-4));
    eq.set_content_top_padding(40.0f);

    // A band pinned at max gain would plot above the reserved top; its handle
    // clamps into the plot and stays grabbable via the exposed hit_test_handle.
    eq.set_bands({{1000.0f, 24.0f, 2.0f, EqCurveView::FilterType::peak, true}});
    auto fs = eq.frequency_scale();
    auto gs = eq.gain_scale();   // padded scale: +24 dB maps to the padded top
    REQUIRE(eq.hit_test_handle({fs.to_x(1000.0f), gs.to_y(24.0f)}) == 0);

    RecordingCanvas c;
    eq.paint(c);
    REQUIRE(c.count(DrawCommand::Type::fill_circle) == 1);
}

TEST_CASE("EqCurveView exposes a distinct wrapping band palette", "[view][eq_curve]") {
    const Color c0 = EqCurveView::band_palette_color(0);
    const Color c1 = EqCurveView::band_palette_color(1);
    REQUIRE_FALSE(c0 == c1);                             // adjacent bands differ
    REQUIRE(EqCurveView::band_palette_color(8) == c0);   // wraps modulo the palette
    REQUIRE(c0.a > 0.0f);                                // opaque — usable as a fill
}

TEST_CASE("EqCurveView clear_spectrum removes the analyzer overlay", "[view][eq_curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 240, 120});
    std::vector<float> mags(32, -12.0f);
    eq.set_spectrum(mags.data(), mags.size());

    RecordingCanvas with_spec;
    eq.paint(with_spec);
    const int with = with_spec.count(DrawCommand::Type::set_fill_gradient_linear);
    REQUIRE(with >= 1);   // the analyzer draws a gradient envelope

    eq.clear_spectrum();
    RecordingCanvas without_spec;
    eq.paint(without_spec);
    // No bands and no spectrum → the gradient envelope is gone.
    REQUIRE(without_spec.count(DrawCommand::Type::set_fill_gradient_linear) < with);
}

// ── EqCurveView ─────────────────────────────────────────────────────────────

TEST_CASE("EqCurveView band management", "[view][eq_curve]") {
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
            {5000.0f, -3.0f, 2.0f, EqCurveView::FilterType::peak, true}
        };
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

TEST_CASE("EqCurveView spectrum overlay", "[view][eq_curve]") {
    EqCurveView eq;
    std::vector<float> spectrum(128, -60.0f);
    eq.set_spectrum(spectrum.data(), spectrum.size());
    eq.clear_spectrum();
    // Should not crash
}

TEST_CASE("EqCurveView hit testing and drag callbacks", "[view][eq_curve][issue-493]") {
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

TEST_CASE("EqCurveView empty hit does not start a drag", "[view][eq_curve][issue-493]") {
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
          "[view][eq_curve][issue-652]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 240, 120});
    eq.set_show_grid(false);
    eq.add_band({500.0f, 12.0f, 1.0f, EqCurveView::FilterType::peak, false});

    pulp::canvas::RecordingCanvas canvas;
    eq.paint(canvas);

    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_circle) == 0);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::stroke_circle) == 0);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) > 0);
}

TEST_CASE("EqCurveView set_band updates valid indices",
          "[view][eq_curve]") {
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

TEST_CASE("EqCurveView paint covers grid spectrum and enabled handles",
          "[view][eq_curve]") {
    EqCurveView eq;
    eq.set_bounds({0, 0, 240, 120});
    const float spectrum[] = {-18.0f, -12.0f, -24.0f, -6.0f};
    eq.set_spectrum(spectrum, sizeof(spectrum) / sizeof(spectrum[0]));
    eq.add_band({1000.0f, 6.0f, 1.0f, EqCurveView::FilterType::peak, true,
                 Color::rgba8(240, 20, 20)});
    eq.set_selected_band(0);

    RecordingCanvas canvas;
    eq.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 1);   // background
    // The spectrum is now a smooth gradient-filled envelope (not per-column
    // bars): its soft vertical wash plus the band's own fill both go through
    // set_fill_gradient_linear, so at least one gradient fill is recorded.
    REQUIRE(canvas.count(DrawCommand::Type::set_fill_gradient_linear) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) > 200);
    REQUIRE(canvas.count(DrawCommand::Type::fill_circle) == 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_circle) == 1);
    REQUIRE(has_fill_color(canvas, Color::rgba8(240, 20, 20)));
}

TEST_CASE("EqCurveView forwards rich mouse events to the base pointer hook",
          "[view][eq_curve]") {
    EqCurveView eq;
    int forwarded = 0;
    eq.on_pointer_event = [&](const MouseEvent& event) {
        ++forwarded;
        REQUIRE(event.pointer_id == 9);
    };

    eq.on_mouse_event(MouseEvent{{8, 9}, {8, 9}, MouseButton::left, 0, 9, 1, true});

    REQUIRE(forwarded == 1);
}

// ── MidiKeyboard ────────────────────────────────────────────────────────────

TEST_CASE("MidiKeyboard note state", "[view][midi_keyboard]") {
    MidiKeyboard kb;

    SECTION("Note on/off") {
        REQUIRE_FALSE(kb.is_note_on(60));
        kb.note_on(60, 0.8f);
        REQUIRE(kb.is_note_on(60));
        kb.note_off(60);
        REQUIRE_FALSE(kb.is_note_on(60));
    }

    SECTION("All notes off") {
        kb.note_on(60);
        kb.note_on(64);
        kb.note_on(67);
        kb.all_notes_off();
        REQUIRE_FALSE(kb.is_note_on(60));
        REQUIRE_FALSE(kb.is_note_on(64));
        REQUIRE_FALSE(kb.is_note_on(67));
    }

    SECTION("Range clamping") {
        kb.set_range(48, 72);
        REQUIRE(kb.first_note() == 48);
        REQUIRE(kb.last_note() == 72);
    }

    SECTION("Out of range note is safe") {
        REQUIRE_FALSE(kb.is_note_on(-1));
        REQUIRE_FALSE(kb.is_note_on(128));
        kb.note_on(-1);  // Should not crash
        kb.note_on(128); // Should not crash
    }
}

TEST_CASE("MidiKeyboard interaction callback", "[view][midi_keyboard]") {
    MidiKeyboard kb;
    kb.set_range(60, 72);
    kb.set_bounds({0, 0, 300, 80});

    int last_note = -1;
    float last_vel = 0;
    kb.on_note_on = [&](int n, float v) { last_note = n; last_vel = v; };

    // Click near the start should hit a key
    kb.on_mouse_down({5, 40});
    REQUIRE(last_note >= 60);
    REQUIRE(last_vel > 0);
}

TEST_CASE("MidiKeyboard vertical drag releases previous notes and misses",
          "[view][midi_keyboard][issue-493]") {
    MidiKeyboard kb;
    kb.set_range(60, 64);
    kb.set_orientation(MidiKeyboard::Orientation::vertical);
    kb.set_bounds({0, 0, 80, 300});

    std::vector<int> note_ons;
    std::vector<int> note_offs;
    kb.on_note_on = [&](int note, float velocity) {
        REQUIRE_THAT(velocity, WithinAbs(0.8, 0.001));
        note_ons.push_back(note);
    };
    kb.on_note_off = [&](int note) { note_offs.push_back(note); };

    kb.on_mouse_down({10, 20});
    REQUIRE(note_ons.size() == 1);
    REQUIRE(note_ons[0] == 60);
    REQUIRE(kb.is_note_on(60));

    kb.on_mouse_drag({10, 110});
    REQUIRE(note_offs.size() == 1);
    REQUIRE(note_offs[0] == 60);
    REQUIRE(note_ons.size() == 2);
    REQUIRE(note_ons[1] == 61);
    REQUIRE_FALSE(kb.is_note_on(60));
    REQUIRE(kb.is_note_on(61));

    kb.on_mouse_drag({90, 310});
    REQUIRE(note_offs.size() == 2);
    REQUIRE(note_offs[1] == 61);
    REQUIRE_FALSE(kb.is_note_on(61));

    kb.on_mouse_up({90, 310});
    REQUIRE(note_offs.size() == 2);
}

TEST_CASE("MidiKeyboard drag releases old notes and supports vertical range",
          "[view][midi_keyboard][issue-652]") {
    MidiKeyboard kb;
    kb.set_range(80, 60);
    REQUIRE(kb.first_note() == 80);
    REQUIRE(kb.last_note() == 80);

    kb.set_range(60, 72);
    kb.set_orientation(MidiKeyboard::Orientation::vertical);
    kb.set_bounds({0, 0, 100, 130});

    std::vector<int> note_ons;
    std::vector<int> note_offs;
    kb.on_note_on = [&](int note, float velocity) {
        note_ons.push_back(note);
        REQUIRE_THAT(velocity, WithinAbs(0.8f, 0.001f));
    };
    kb.on_note_off = [&](int note) { note_offs.push_back(note); };

    kb.on_mouse_down({10, 12});
    REQUIRE(note_ons == std::vector<int>{61});
    REQUIRE(kb.is_note_on(61));

    kb.on_mouse_drag({10, 120});
    REQUIRE(note_offs == std::vector<int>{61});
    REQUIRE_FALSE(kb.is_note_on(61));
    REQUIRE_FALSE(note_ons.empty());
    REQUIRE(kb.is_note_on(note_ons.back()));

    kb.on_mouse_up({10, 120});
    REQUIRE_FALSE(kb.is_note_on(note_ons.back()));
    REQUIRE(note_offs.back() == note_ons.back());
}

TEST_CASE("MidiKeyboard paint emits note names and active highlight color",
          "[view][midi_keyboard][issue-493]") {
    MidiKeyboard kb;
    kb.set_range(60, 64);
    kb.set_bounds({0, 0, 300, 80});
    kb.set_show_note_names(true);
    kb.set_highlight_color(Color::rgba8(255, 0, 0));
    kb.note_on(60);

    RecordingCanvas canvas;
    kb.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 5);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_rounded_rect) == 3);
    REQUIRE(has_text(canvas, "C4"));
    REQUIRE(has_fill_color(canvas, Color::rgba8(255, 0, 0)));
}

TEST_CASE("MidiKeyboard forwards rich mouse events to the base pointer hook",
          "[view][midi_keyboard]") {
    MidiKeyboard kb;
    int forwarded = 0;
    Point last{};
    kb.on_pointer_event = [&](const MouseEvent& event) {
        ++forwarded;
        last = event.position;
    };

    kb.on_mouse_event(MouseEvent{{12, 34}, {12, 34}, MouseButton::left, 0, 7, 1, true});

    REQUIRE(forwarded == 1);
    REQUIRE(last.x == 12.0f);
    REQUIRE(last.y == 34.0f);
}

TEST_CASE("MidiKeyboard overlapping black keys win hit testing",
          "[view][midi_keyboard]") {
    MidiKeyboard kb;
    kb.set_range(60, 61);
    kb.set_bounds({0, 0, 100, 80});
    std::vector<int> notes;
    kb.on_note_on = [&](int note, float) { notes.push_back(note); };

    kb.on_mouse_down({80, 20});

    REQUIRE(notes == std::vector<int>{61});
    REQUIRE(kb.is_note_on(61));
    REQUIRE_FALSE(kb.is_note_on(60));
}

// ── ColorPicker ─────────────────────────────────────────────────────────────

TEST_CASE("ColorPicker set/get color", "[view][color_picker]") {
    ColorPicker picker;

    picker.set_color(Color::rgba8(255, 128, 0));
    REQUIRE(picker.color().r8() == 255);
    REQUIRE(picker.color().g8() == 128);
    REQUIRE(picker.color().b8() == 0);
}

TEST_CASE("ColorPicker hex round-trip", "[view][color_picker]") {
    ColorPicker picker;
    picker.set_hex("#ff6600");
    REQUIRE(picker.hex() == "#ff6600");
    REQUIRE(picker.color().r8() == 0xFF);
    REQUIRE(picker.color().g8() == 0x66);
    REQUIRE(picker.color().b8() == 0x00);
}

TEST_CASE("ColorPicker hex alpha and malformed input are stable",
          "[view][color_picker][issue-493]") {
    ColorPicker picker;
    picker.set_hex("#33669980");
    REQUIRE(picker.hex() == "#33669980");
    REQUIRE(picker.color().r8() == 0x33);
    REQUIRE(picker.color().g8() == 0x66);
    REQUIRE(picker.color().b8() == 0x99);
    REQUIRE(picker.color().a8() == 0x80);

    const auto before = picker.hex();
    picker.set_hex("336699");
    REQUIRE(picker.hex() == before);
    picker.set_hex("#3366991234");
    REQUIRE(picker.hex() == before);
    REQUIRE_NOTHROW(picker.set_hex("#zzzzzz"));
    REQUIRE(picker.hex() == before);
}

TEST_CASE("ColorPicker HSL round-trip", "[view][color_picker]") {
    ColorPicker picker;
    HSL hsl{120.0f, 1.0f, 0.5f};
    picker.set_hsl(hsl);
    REQUIRE(picker.color().g > picker.color().r);
    REQUIRE(picker.color().g > picker.color().b);
}

TEST_CASE("ColorPicker swatches", "[view][color_picker]") {
    ColorPicker picker;
    picker.set_swatches({
        Color::rgba8(255, 0, 0),
        Color::rgba8(0, 255, 0),
        Color::rgba8(0, 0, 255)
    });
    REQUIRE(picker.swatches().size() == 3);
}

TEST_CASE("ColorPicker mouse editing updates SL hue alpha and swatches",
          "[view][color_picker][issue-493]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 280});
    picker.set_hex("#000000ff");

    int changes = 0;
    picker.on_change = [&](Color) { ++changes; };

    picker.on_mouse_down({90, 90});
    REQUIRE(changes == 1);
    REQUIRE(picker.color().r8() > 0);

    picker.on_mouse_down({176, 184});
    REQUIRE(changes == 2);
    REQUIRE_THAT(picker.hsl().h, WithinAbs(360.0, 0.1));

    picker.set_show_alpha(true);
    picker.on_mouse_down({4, 212});
    REQUIRE(changes == 3);
    REQUIRE(picker.color().a8() == 0);
    picker.on_mouse_drag({176, 212});
    REQUIRE(changes == 4);
    REQUIRE(picker.color().a8() == 255);
    picker.on_mouse_up({176, 212});
    picker.on_mouse_drag({4, 212});
    REQUIRE(changes == 4);

    picker.set_show_alpha(false);
    picker.set_swatches({
        Color::rgba8(255, 0, 0),
        Color::rgba8(0, 255, 0),
    });

    picker.on_mouse_down({32, 216});
    REQUIRE(changes == 5);
    REQUIRE(picker.color().g8() == 255);
    REQUIRE(picker.color().r8() == 0);
}

TEST_CASE("ColorPicker paint positions alpha cursor from normalized alpha",
          "[view][color_picker][issue-493]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 280});
    picker.set_show_alpha(true);
    picker.set_hex("#33669980");

    pulp::canvas::RecordingCanvas canvas;
    picker.paint(canvas);

    bool found_alpha_cursor = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != pulp::canvas::DrawCommand::Type::fill_rect) continue;
        if (command.f[1] < 209.0f || command.f[1] > 211.0f) continue;
        if (command.f[2] != 4.0f || command.f[3] != 24.0f) continue;

        found_alpha_cursor = true;
        REQUIRE(command.f[0] > 86.0f);
        REQUIRE(command.f[0] < 91.0f);
    }
    REQUIRE(found_alpha_cursor);
}

TEST_CASE("ColorPicker paint draws configured swatches",
          "[view][color_picker]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 260});
    picker.set_swatches({
        Color::rgba8(255, 0, 0),
        Color::rgba8(0, 255, 0),
        Color::rgba8(0, 0, 255),
    });

    RecordingCanvas canvas;
    picker.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) >= 5);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_rounded_rect) >= 4);
    REQUIRE(has_fill_color(canvas, Color::rgba8(255, 0, 0)));
    REQUIRE(has_fill_color(canvas, Color::rgba8(0, 255, 0)));
    REQUIRE(has_fill_color(canvas, Color::rgba8(0, 0, 255)));
}

TEST_CASE("ColorPicker forwards rich mouse events to the base pointer hook",
          "[view][color_picker]") {
    ColorPicker picker;
    int forwarded = 0;
    picker.on_pointer_event = [&](const MouseEvent& event) {
        ++forwarded;
        REQUIRE(event.pointer_id == 3);
        REQUIRE(event.is_down);
    };

    picker.on_mouse_event(MouseEvent{{1, 2}, {1, 2}, MouseButton::left, 0, 3, 1, true});

    REQUIRE(forwarded == 1);
}

TEST_CASE("ColorPicker hidden alpha bar ignores alpha-region input",
          "[view][color_picker]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 280});
    picker.set_show_alpha(false);
    picker.set_hex("#33669980");
    int changes = 0;
    picker.on_change = [&](Color) { ++changes; };

    picker.on_mouse_down({60, 212});
    picker.on_mouse_drag({176, 212});
    picker.on_mouse_up({176, 212});

    REQUIRE(changes == 0);
    REQUIRE(picker.hex() == "#33669980");
}

TEST_CASE("ColorPicker mode and outside mouse input are stable",
          "[view][color_picker][issue-652]") {
    ColorPicker picker;
    picker.set_bounds({0, 0, 200, 260});
    picker.set_mode(ColorPicker::Mode::hex_only);
    REQUIRE(picker.mode() == ColorPicker::Mode::hex_only);

    picker.set_color(Color::rgba8(10, 20, 30));
    auto before = picker.hex();
    int changes = 0;
    picker.on_change = [&](Color) { ++changes; };

    picker.on_mouse_down({300, 300});
    picker.on_mouse_drag({4, 4});
    picker.on_mouse_up({4, 4});

    REQUIRE(changes == 0);
    REQUIRE(picker.hex() == before);
}

// ── FileDropZone ────────────────────────────────────────────────────────────

TEST_CASE("FileDropZone extension filtering", "[view][file_drop]") {
    FileDropZone zone;
    zone.set_accepted_extensions({".wav", ".aiff"});

    SECTION("Valid extension drag") {
        zone.drag_enter({"test.wav"});
        REQUIRE(zone.is_drag_over());
        REQUIRE(zone.is_drag_valid());
        zone.drag_leave();
        REQUIRE_FALSE(zone.is_drag_over());
    }

    SECTION("Invalid extension drag") {
        zone.drag_enter({"test.exe"});
        REQUIRE(zone.is_drag_over());
        REQUIRE_FALSE(zone.is_drag_valid());
    }

    SECTION("Mixed extensions") {
        zone.drag_enter({"test.wav", "test.exe"});
        REQUIRE_FALSE(zone.is_drag_valid());
    }

    SECTION("Case insensitive") {
        zone.drag_enter({"test.WAV"});
        REQUIRE(zone.is_drag_valid());
    }
}

TEST_CASE("FileDropZone drop callback", "[view][file_drop]") {
    FileDropZone zone;
    zone.set_accepted_extensions({".wav"});

    std::vector<std::string> dropped;
    zone.on_drop = [&](const auto& paths) { dropped = paths; };

    zone.drop({"a.wav", "b.exe", "c.wav"});
    REQUIRE(dropped.size() == 2);
    REQUIRE(dropped[0] == "a.wav");
    REQUIRE(dropped[1] == "c.wav");
}

TEST_CASE("FileDropZone empty extensions accepts all", "[view][file_drop]") {
    FileDropZone zone;
    zone.drag_enter({"anything.xyz"});
    REQUIRE(zone.is_drag_valid());
}

TEST_CASE("FileDropZone rejects extensionless files when extensions are required",
          "[view][file_drop]") {
    FileDropZone zone;
    zone.set_accepted_extensions({".wav"});

    zone.drag_enter({"README"});

    REQUIRE(zone.is_drag_over());
    REQUIRE_FALSE(zone.is_drag_valid());
}

TEST_CASE("FileDropZone paint reflects idle valid and invalid drag states",
          "[view][file_drop][issue-652]") {
    FileDropZone zone;
    zone.set_bounds({0, 0, 180, 120});
    zone.set_label("Idle");
    zone.set_hover_label("Ready");
    zone.set_accepted_extensions({".wav"});
    auto fill_text_count = [](const pulp::canvas::RecordingCanvas& canvas,
                              const std::string& text) {
        int count = 0;
        for (const auto& command : canvas.commands()) {
            if (command.type == pulp::canvas::DrawCommand::Type::fill_text &&
                command.text == text) {
                ++count;
            }
        }
        return count;
    };

    pulp::canvas::RecordingCanvas idle;
    zone.paint(idle);
    REQUIRE(idle.count(pulp::canvas::DrawCommand::Type::fill_text) == 1);
    REQUIRE(fill_text_count(idle, "Idle") == 1);
    REQUIRE(idle.count(pulp::canvas::DrawCommand::Type::stroke_line) == 3);

    zone.drag_enter({"take.wav"});
    pulp::canvas::RecordingCanvas valid;
    zone.paint(valid);
    REQUIRE(fill_text_count(valid, "Ready") == 1);

    zone.drag_enter({"take.txt"});
    pulp::canvas::RecordingCanvas invalid;
    zone.paint(invalid);
    REQUIRE(fill_text_count(invalid, "Idle") == 1);

    zone.set_icon_style(FileDropZone::IconStyle::none);
    pulp::canvas::RecordingCanvas no_icon;
    zone.paint(no_icon);
    REQUIRE(no_icon.count(pulp::canvas::DrawCommand::Type::stroke_line) == 0);
}

TEST_CASE("FileDropZone invalid or empty drops do not call callback",
          "[view][file_drop][issue-652]") {
    FileDropZone zone;
    zone.set_accepted_extensions({".wav"});

    int drops = 0;
    zone.on_drop = [&](const std::vector<std::string>&) { ++drops; };

    zone.drag_enter({"notes.txt"});
    REQUIRE(zone.is_drag_over());
    REQUIRE_FALSE(zone.is_drag_valid());

    zone.drop({"notes.txt"});
    zone.drop({});
    REQUIRE(drops == 0);
    REQUIRE_FALSE(zone.is_drag_over());
    REQUIRE_FALSE(zone.is_drag_valid());
}

TEST_CASE("FileDropZone paint covers idle valid invalid and no-icon states",
          "[view][file_drop][issue-493]") {
    FileDropZone zone;
    zone.set_bounds({0, 0, 200, 120});
    zone.set_label("Drop audio");
    zone.set_hover_label("Release audio");
    zone.set_accepted_extensions({".wav"});

    zone.set_icon_style(FileDropZone::IconStyle::none);
    RecordingCanvas idle;
    zone.paint(idle);
    REQUIRE(has_text(idle, "Drop audio"));
    REQUIRE(idle.count(DrawCommand::Type::stroke_line) == 0);

    zone.set_icon_style(FileDropZone::IconStyle::upload);
    zone.drag_enter({"sound.wav"});
    RecordingCanvas valid;
    zone.paint(valid);
    REQUIRE(has_text(valid, "Release audio"));
    REQUIRE(valid.count(DrawCommand::Type::stroke_line) == 3);

    zone.drag_enter({"sound.txt"});
    RecordingCanvas invalid;
    zone.paint(invalid);
    REQUIRE(has_text(invalid, "Drop audio"));
    REQUIRE(invalid.count(DrawCommand::Type::stroke_line) == 3);
}

// ── SplitView ───────────────────────────────────────────────────────────────

TEST_CASE("SplitView basic setup", "[view][split_view]") {
    SplitView split;
    split.set_bounds({0, 0, 400, 300});

    auto first = std::make_unique<View>();
    auto second = std::make_unique<View>();
    auto* f = first.get();
    auto* s = second.get();

    split.set_first(std::move(first));
    split.set_second(std::move(second));

    REQUIRE(split.first() == f);
    REQUIRE(split.second() == s);

    split.set_split_fraction(0.3f);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.3, 0.001));

    split.layout_children();
    // First pane should be about 30% of width
    REQUIRE(f->bounds().width < 200);
}

TEST_CASE("SplitView split fraction clamped", "[view][split_view]") {
    SplitView split;
    split.set_split_fraction(-0.5f);
    REQUIRE(split.split_fraction() >= 0.0f);
    split.set_split_fraction(1.5f);
    REQUIRE(split.split_fraction() <= 1.0f);
}

TEST_CASE("SplitView orientation", "[view][split_view]") {
    SplitView split;
    split.set_orientation(SplitView::Orientation::vertical);
    REQUIRE(split.orientation() == SplitView::Orientation::vertical);
}

TEST_CASE("SplitView drag handling clamps to pane minimums and ignores misses",
          "[view][split_view][issue-493]") {
    SplitView split;
    split.set_bounds({0, 0, 400, 200});
    split.set_first(std::make_unique<View>());
    split.set_second(std::make_unique<View>());
    split.set_min_first_size(80.0f);
    split.set_min_second_size(80.0f);
    split.set_split_fraction(0.5f);
    split.layout_children();

    int changes = 0;
    float last_fraction = 0.0f;
    split.on_split_changed = [&](float fraction) {
        ++changes;
        last_fraction = fraction;
    };

    split.on_mouse_drag({10, 100});
    REQUIRE(changes == 0);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.5, 0.001));

    split.on_mouse_down({20, 20});
    split.on_mouse_drag({300, 100});
    REQUIRE(changes == 0);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.5, 0.001));

    split.on_mouse_down({200, 100});
    split.on_mouse_drag({10, 100});
    REQUIRE(changes == 1);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.2, 0.001));
    REQUIRE_THAT(last_fraction, WithinAbs(0.2, 0.001));

    split.on_mouse_drag({390, 100});
    REQUIRE(changes == 2);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.8, 0.001));
    split.on_mouse_up({390, 100});

    split.on_mouse_drag({100, 100});
    REQUIRE(changes == 2);

    split.set_orientation(SplitView::Orientation::vertical);
    split.set_split_fraction(0.5f);
    split.layout_children();
    split.on_mouse_down({200, 100});
    split.on_mouse_drag({200, 190});
    REQUIRE(changes == 3);
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.6, 0.001));
}

TEST_CASE("SplitView paint emits horizontal and vertical divider grips",
          "[view][split_view][issue-493]") {
    SplitView split;
    split.set_bounds({0, 0, 300, 180});

    pulp::canvas::RecordingCanvas canvas;
    split.paint(canvas);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_circle) == 3);

    canvas.clear();
    split.set_orientation(SplitView::Orientation::vertical);
    split.paint(canvas);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_circle) == 3);
}

TEST_CASE("SplitView can replace panes and lays out with custom divider width",
          "[view][split_view]") {
    SplitView split;
    split.set_bounds({0, 0, 300, 120});
    split.set_split_fraction(0.25f);
    split.set_divider_width(10.0f);

    auto first = std::make_unique<View>();
    auto second = std::make_unique<View>();
    auto* original_first = first.get();
    auto* second_ptr = second.get();
    split.set_first(std::move(first));
    split.set_second(std::move(second));
    split.layout_children();

    REQUIRE(split.first() == original_first);
    REQUIRE(split.second() == second_ptr);
    REQUIRE_THAT(split.divider_width(), WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(original_first->bounds().width, WithinAbs(70.0f, 0.001f));
    REQUIRE_THAT(second_ptr->bounds().x, WithinAbs(80.0f, 0.001f));

    auto replacement = std::make_unique<View>();
    auto* replacement_ptr = replacement.get();
    split.set_first(std::move(replacement));
    split.layout_children();

    REQUIRE(split.first() == replacement_ptr);
    REQUIRE(split.second() == second_ptr);
    REQUIRE_THAT(replacement_ptr->bounds().width, WithinAbs(70.0f, 0.001f));

    split.set_second(nullptr);
    REQUIRE(split.second() == nullptr);
    split.layout_children();
    REQUIRE(split.first() == replacement_ptr);
}

TEST_CASE("SplitView forwards rich mouse events to the base pointer hook",
          "[view][split_view]") {
    SplitView split;
    int forwarded = 0;
    split.on_pointer_event = [&](const MouseEvent& event) {
        ++forwarded;
        REQUIRE(event.position.x == 5.0f);
    };

    split.on_mouse_event(MouseEvent{{5, 6}, {5, 6}, MouseButton::left, 0, 0, 1, true});

    REQUIRE(forwarded == 1);
}

// ── PropertyList ────────────────────────────────────────────────────────────

TEST_CASE("PropertyList basic operations", "[view][property_list]") {
    PropertyList list;

    std::vector<PropertyList::Property> props = {
        {"name", "Name", std::string("MyPlugin"), false, "General"},
        {"version", "Version", std::string("1.0.0"), true, "General"},
        {"gain", "Gain", 0.5f, false, "Audio"},
        {"bypass", "Bypass", false, false, "Audio"},
        {"color", "Color", Color::rgba8(255, 0, 0), false, "Visual"},
    };

    list.set_properties(props);
    REQUIRE(list.properties().size() == 5);

    auto* found = list.find_property("gain");
    REQUIRE(found != nullptr);
    REQUIRE(std::get<float>(found->value) == 0.5f);

    list.set_value("gain", 0.8f);
    found = list.find_property("gain");
    REQUIRE_THAT(std::get<float>(found->value), WithinAbs(0.8, 0.001));
}

TEST_CASE("PropertyList find missing returns nullptr", "[view][property_list]") {
    PropertyList list;
    REQUIRE(list.find_property("nonexistent") == nullptr);
}

TEST_CASE("PropertyList intrinsic height", "[view][property_list]") {
    PropertyList list;
    list.set_properties({
        {"a", "A", std::string("x"), false, ""},
        {"b", "B", std::string("y"), false, ""},
    });
    REQUIRE(list.intrinsic_height() > 0);
}

TEST_CASE("PropertyList mouse editing toggles writable booleans only",
          "[view][property_list][issue-493]") {
    PropertyList list;
    list.set_bounds({0, 0, 240, 160});
    list.set_properties({
        {"enabled", "Enabled", false, false, "General"},
        {"locked", "Locked", true, true, "General"},
        {"name", "Name", std::string("Osc"), false, "Info"},
    });

    int changes = 0;
    std::string changed_key;
    bool changed_bool = false;
    list.on_change = [&](const std::string& key, PropertyList::PropertyValue value) {
        ++changes;
        changed_key = key;
        if (std::holds_alternative<bool>(value))
            changed_bool = std::get<bool>(value);
    };

    list.on_mouse_down({12, 30});
    auto* enabled = list.find_property("enabled");
    REQUIRE(enabled != nullptr);
    REQUIRE(std::get<bool>(enabled->value));
    REQUIRE(changes == 1);
    REQUIRE(changed_key == "enabled");
    REQUIRE(changed_bool);

    list.on_mouse_down({12, 60});
    auto* locked = list.find_property("locked");
    REQUIRE(locked != nullptr);
    REQUIRE(std::get<bool>(locked->value));
    REQUIRE(changes == 1);

    list.on_mouse_down({12, 110});
    REQUIRE(changes == 1);

    list.on_mouse_down({12, 500});
    REQUIRE(changes == 1);
}

TEST_CASE("PropertyList paints categories and scalar value variants",
          "[view][property_list][issue-493]") {
    PropertyList list;
    list.set_bounds({0, 0, 260, 180});
    list.set_row_height(20.0f);
    list.set_label_width_fraction(0.5f);
    list.set_properties({
        {"name", "", std::string("Osc"), false, "General"},
        {"gain", "Gain", 0.5f, false, "General"},
        {"voices", "Voices", 8, false, "Synth"},
        {"enabled", "Enabled", true, false, "Synth"},
    });

    const auto with_categories = list.intrinsic_height();
    list.set_show_categories(false);
    REQUIRE_THAT(list.intrinsic_height(), WithinAbs(80.0, 0.001));
    REQUIRE(with_categories > list.intrinsic_height());

    pulp::canvas::RecordingCanvas canvas;
    list.paint(canvas);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::stroke_line) == 4);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_text) == 8);

    bool saw_key_label = false;
    bool saw_float_value = false;
    bool saw_int_value = false;
    bool saw_bool_value = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != pulp::canvas::DrawCommand::Type::fill_text)
            continue;
        saw_key_label = saw_key_label || command.text == "name";
        saw_float_value = saw_float_value || command.text == "0.50";
        saw_int_value = saw_int_value || command.text == "8";
        saw_bool_value = saw_bool_value || command.text == "true";
    }
    REQUIRE(saw_key_label);
    REQUIRE(saw_float_value);
    REQUIRE(saw_int_value);
    REQUIRE(saw_bool_value);

    canvas.clear();
    list.set_show_categories(true);
    list.paint(canvas);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_text) == 10);
}

TEST_CASE("PropertyList paints color values and selected row highlight",
          "[view][property_list]") {
    PropertyList list;
    list.set_bounds({0, 0, 260, 100});
    list.set_row_height(24.0f);
    list.set_show_categories(false);
    list.set_properties({
        {"accent", "Accent", Color::rgba8(0x33, 0x66, 0x99), true, ""},
        {"enabled", "Enabled", false, false, ""},
    });

    list.on_mouse_down({12, 10});

    RecordingCanvas canvas;
    list.paint(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) == 2);
    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(has_text(canvas, "#336699"));
    REQUIRE(has_text(canvas, "false"));
    REQUIRE(has_fill_color(canvas, Color::rgba8(0x33, 0x66, 0x99)));
}

// ── Breadcrumb ──────────────────────────────────────────────────────────────

TEST_CASE("Breadcrumb push and pop", "[view][breadcrumb]") {
    Breadcrumb bc;

    bc.push({"Home", "home"});
    bc.push({"Settings", "settings"});
    bc.push({"Audio", "audio"});

    REQUIRE(bc.items().size() == 3);

    auto popped = bc.pop();
    REQUIRE(popped.label == "Audio");
    REQUIRE(bc.items().size() == 2);
}

TEST_CASE("Breadcrumb pop_to", "[view][breadcrumb]") {
    Breadcrumb bc;
    bc.push({"Home", "home"});
    bc.push({"A", "a"});
    bc.push({"B", "b"});
    bc.push({"C", "c"});

    bc.pop_to(1);
    REQUIRE(bc.items().size() == 2);
    REQUIRE(bc.items().back().label == "A");
}

TEST_CASE("Breadcrumb set_items replaces", "[view][breadcrumb]") {
    Breadcrumb bc;
    bc.set_items({{"Root", "root"}, {"Child", "child"}});
    REQUIRE(bc.items().size() == 2);
}

TEST_CASE("Breadcrumb navigate callback", "[view][breadcrumb]") {
    Breadcrumb bc;
    bc.set_bounds({0, 0, 400, 32});
    bc.push({"Home", "home"});
    bc.push({"Settings", "settings"});

    size_t nav_idx = 999;
    bc.on_navigate = [&](size_t idx, const Breadcrumb::Item&) { nav_idx = idx; };

    bc.on_mouse_down({10, 16}); // Should hit first item
    REQUIRE(nav_idx == 0);
}

TEST_CASE("Breadcrumb separator", "[view][breadcrumb]") {
    Breadcrumb bc;
    REQUIRE(bc.separator() == "/");
    bc.set_separator(">");
    REQUIRE(bc.separator() == ">");
}

TEST_CASE("Breadcrumb empty and out-of-range interactions are stable",
          "[view][breadcrumb]") {
    Breadcrumb bc;
    auto empty = bc.pop();
    REQUIRE(empty.label.empty());
    REQUIRE(empty.id.empty());

    bc.set_items({{"Home", "home"}, {"Settings", "settings"}});
    bc.pop_to(99);
    REQUIRE(bc.items().size() == 2);

    int navigate_calls = 0;
    bc.on_navigate = [&](size_t, const Breadcrumb::Item&) { ++navigate_calls; };
    bc.on_mouse_down({200, 16});
    REQUIRE(navigate_calls == 0);
}

TEST_CASE("Breadcrumb navigation targets later items and ignores separators",
          "[view][breadcrumb]") {
    Breadcrumb bc;
    bc.set_separator(">");
    bc.set_items({{"Home", "home"}, {"Settings", "settings"}, {"Audio", "audio"}});

    size_t nav_idx = 999;
    std::string nav_id;
    int navigate_calls = 0;
    bc.on_navigate = [&](size_t idx, const Breadcrumb::Item& item) {
        nav_idx = idx;
        nav_id = item.id;
        ++navigate_calls;
    };

    bc.on_mouse_down({60, 16});
    REQUIRE(navigate_calls == 1);
    REQUIRE(nav_idx == 1);
    REQUIRE(nav_id == "settings");

    bc.on_mouse_down({45, 16});
    REQUIRE(navigate_calls == 1);
}

TEST_CASE("Breadcrumb paint emits background, items, and separators",
          "[view][breadcrumb]") {
    Breadcrumb bc;
    bc.set_bounds({0, 0, 240, 32});
    bc.set_separator(">");
    bc.set_items({{"Home", "home"}, {"Settings", "settings"}, {"Audio", "audio"}});

    pulp::canvas::RecordingCanvas canvas;
    bc.paint(canvas);

    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::set_font) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::set_text_align) == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_text) == 5);

    int home_text = 0;
    int separator_text = 0;
    for (const auto& command : canvas.commands()) {
        if (command.type != pulp::canvas::DrawCommand::Type::fill_text)
            continue;
        if (command.text == "Home")
            ++home_text;
        if (command.text == ">")
            ++separator_text;
    }

    REQUIRE(home_text == 1);
    REQUIRE(separator_text == 2);
}

// ── ThemeEditor ────────────────────────────────────────────────────────────

TEST_CASE("ThemeEditor covers missing selection empty theme and selected paint",
          "[view][theme-editor][issue-652]") {
    ThemeEditor editor;
    editor.set_bounds({0, 0, 360, 120});

    editor.select_token("missing");
    REQUIRE(editor.selected_token() == "missing");
    REQUIRE(editor.token_names().empty());

    pulp::canvas::RecordingCanvas empty;
    editor.paint_all(empty);
    REQUIRE(empty.count(pulp::canvas::DrawCommand::Type::fill_text) == 1);

    Theme theme;
    theme.colors["accent.primary"] = Color::rgba8(1, 2, 3);
    theme.colors["bg.primary"] = Color::rgba8(4, 5, 6);
    editor.set_theme(theme);
    editor.select_token("accent.primary");

    pulp::canvas::RecordingCanvas selected;
    editor.paint_all(selected);

    REQUIRE(selected.count(pulp::canvas::DrawCommand::Type::fill_rounded_rect) == 2);
    REQUIRE(selected.count(pulp::canvas::DrawCommand::Type::stroke_rounded_rect) == 1);
    REQUIRE(selected.count(pulp::canvas::DrawCommand::Type::fill_text) == 3);
    REQUIRE(editor.export_json().find("accent.primary") != std::string::npos);
}

TEST_CASE("ThemeEditor wraps swatches when the row is full",
          "[view][theme-editor]") {
    ThemeEditor editor;
    editor.set_bounds({0, 0, 80, 160});

    Theme theme;
    theme.colors["accent.primary"] = Color::rgba8(1, 2, 3);
    theme.colors["accent.secondary"] = Color::rgba8(4, 5, 6);
    theme.colors["bg.primary"] = Color::rgba8(7, 8, 9);
    editor.set_theme(theme);

    RecordingCanvas canvas;
    editor.paint_all(canvas);

    REQUIRE(canvas.count(DrawCommand::Type::fill_rounded_rect) == 3);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 4);
    REQUIRE(has_text(canvas, "Theme Editor"));
}

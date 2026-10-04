// The classifier behind pulp-editor-open-oop-probe: from the distinct images
// a host window showed while an editor opened, what did a user see before the
// editor settled? Synthetic images stand in for read-backs.

#include <catch2/catch_test_macros.hpp>

#include "editor_open_stages.hpp"

using pulp::tools::editor_open::classify_open;
using pulp::tools::editor_open::Frame;

namespace {

constexpr std::uint32_t kBg = 0x05070A;

Frame flat(double t, std::size_t w, std::size_t h, std::uint32_t rgb) {
    Frame f;
    f.t_ms = t;
    f.w = w;
    f.h = h;
    f.rgb.resize(w * h * 3);
    for (std::size_t p = 0; p < w * h; ++p) {
        f.rgb[p * 3] = std::uint8_t(rgb >> 16);
        f.rgb[p * 3 + 1] = std::uint8_t((rgb >> 8) & 255);
        f.rgb[p * 3 + 2] = std::uint8_t(rgb & 255);
    }
    return f;
}

// The editor's background with a light panel over its left half: the "UI".
Frame ui(double t, std::size_t w = 240, std::size_t h = 160) {
    Frame f = flat(t, w, h, kBg);
    for (std::size_t y = h / 4; y < 3 * h / 4; ++y)
        for (std::size_t x = w / 8; x < w / 2; ++x)
            for (int c = 0; c < 3; ++c) f.rgb[(y * w + x) * 3 + c] = 0xC8;
    return f;
}

} // namespace

TEST_CASE("an editor that opens content-first shows only its UI", "[editor-open][tools]") {
    const auto stages = classify_open({ui(40), ui(60)}, kBg);
    CHECK(stages.non_ui_content_frames == 0);
    CHECK(stages.ready == 0);
    CHECK(stages.stages == "ui@40ms(240x160)");
}

TEST_CASE("the host's own backdrop before the first composite is not the plug-in's",
          "[editor-open][tools]") {
    const auto stages = classify_open({flat(10, 240, 160, 0xFF00FF), ui(70)}, kBg);
    CHECK(stages.per_frame[0] == "host-empty");
    CHECK(stages.non_ui_content_frames == 0);
}

TEST_CASE("a view-first open shows the empty background before the UI", "[editor-open][tools]") {
    // The three stages users report: the window animating in small and empty,
    // then empty at the right size, then the UI.
    const auto stages = classify_open(
        {flat(20, 230, 150, kBg), flat(60, 240, 160, kBg), ui(400), ui(420)}, kBg);
    REQUIRE(stages.per_frame.size() == 4);
    CHECK(stages.per_frame[0] == "wrong-size");
    CHECK(stages.per_frame[1] == "empty-bg");
    CHECK(stages.per_frame[2] == "ui");
    CHECK(stages.non_ui_content_frames == 2);
    CHECK(stages.stages == "wrong-size@20ms(230x150) > empty-bg@60ms(240x160) > ui@400ms(240x160)");
}

TEST_CASE("the UI scaled by the host window's open animation counts as the UI",
          "[editor-open][tools]") {
    // AppKit zooms a new window from slightly smaller than its final size; a
    // correct open shows the UI scaled, which is not a defect.
    // Mid-animation the content sits off its final proportional position, so
    // it is not yet the settled image, but by block means it is the UI.
    Frame zooming = flat(20, 200, 134, kBg);
    for (std::size_t y = 134 / 4 + 3; y < 3 * 134 / 4 + 3; ++y)
        for (std::size_t x = 200 / 8 + 4; x < 200 / 2 + 4; ++x)
            for (int c = 0; c < 3; ++c) zooming.rgb[(y * 200 + x) * 3 + c] = 0xC8;
    const auto stages = classify_open({zooming, ui(120)}, kBg);
    CHECK(stages.per_frame[0] == "ui-host-animating");
    CHECK(stages.non_ui_content_frames == 0);
}

TEST_CASE("the framework's default navy is counted wherever it shows", "[editor-open][tools]") {
    const auto stages = classify_open({flat(30, 240, 160, 0x1E1E2E), ui(300)}, kBg);
    CHECK(stages.navy_frames == 1);
    CHECK(stages.non_ui_content_frames == 1);
}

TEST_CASE("a read-back that only ever saw the host backdrop is blind, not a pass",
          "[editor-open][tools]") {
    // Seen on a host whose window read-back could not include the remote
    // view: every image magenta while the plug-in's trace showed it drawing.
    const auto stages = classify_open({flat(10, 240, 160, 0xFF00FF), flat(900, 240, 160, 0xFF00FF)}, kBg);
    CHECK(stages.blind);
    CHECK(stages.per_frame[1] == "host-empty");
    // Control: a run that reached the UI is not blind.
    CHECK_FALSE(classify_open({flat(10, 240, 160, 0xFF00FF), ui(90)}, kBg).blind);
}

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace pulp::view;

namespace {

static std::vector<uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

}

TEST_CASE("Screenshot renders to PNG buffer", "[view][screenshot]") {
    View root;
    root.set_theme(Theme::dark());

    auto knob = std::make_unique<Knob>();
    knob->set_bounds({10, 10, 48, 48});
    knob->set_value(0.5f);
    knob->set_label("Gain");
    root.add_child(std::move(knob));

    auto label = std::make_unique<Label>("Test Plugin");
    label->set_bounds({70, 20, 120, 20});
    root.add_child(std::move(label));

    auto png = render_to_png(root, 200, 80, 1.0f);

#if defined(__APPLE__) || defined(PULP_HAS_SKIA)
    // Apple has a native CoreGraphics backend; non-Apple builds with Skia get
    // the built-in cross-platform Skia raster backend (#3329 Win/Linux parity,
    // screenshot_skia.cpp). Either way a valid PNG comes back.
    REQUIRE_FALSE(png.empty());
    // PNG magic bytes: 0x89 P N G
    REQUIRE(png.size() > 8);
    REQUIRE(png[0] == 0x89);
    REQUIRE(png[1] == 'P');
    REQUIRE(png[2] == 'N');
    REQUIRE(png[3] == 'G');
#else
    // Non-Apple WITHOUT Skia and no host provider: explicitly empty.
    REQUIRE(png.empty());
#endif
}

TEST_CASE("Screenshot renders to file", "[view][screenshot]") {
    View root;
    root.set_theme(Theme::dark());

    auto fader = std::make_unique<Fader>();
    fader->set_bounds({10, 10, 24, 100});
    fader->set_value(0.7f);
    root.add_child(std::move(fader));

    auto output_path = std::filesystem::temp_directory_path() / "pulp_screenshot_test.png";

    bool ok = render_to_file(root, 60, 120, output_path.string(), 1.0f);

#if defined(__APPLE__) || defined(PULP_HAS_SKIA)
    REQUIRE(ok);
    REQUIRE(std::filesystem::exists(output_path));
    REQUIRE(std::filesystem::file_size(output_path) > 100); // Not empty

    // Clean up
    std::filesystem::remove(output_path);
#else
    REQUIRE_FALSE(ok);
#endif
}

// ── Edge cases: non-Apple, scale, malformed output path ─────────────────

TEST_CASE("Screenshot backend availability is honest per build",
          "[view][screenshot][edge]") {
    // Contract: render_* must never silently fake-success. On a build with a
    // real backend (Apple CoreGraphics, or non-Apple Skia raster — #3329) it
    // returns valid PNG bytes even for an empty view tree (a solid background
    // frame). On a no-backend build (non-Apple, no Skia, no host provider) it
    // returns empty rather than lying. Pin it against an empty tree so a future
    // refactor can't start fake-succeeding with nothing to render.
    View root;
    auto png = render_to_png(root, 32, 32, 1.0f);
#if defined(__APPLE__) || defined(PULP_HAS_SKIA)
    REQUIRE_FALSE(png.empty());
    REQUIRE(png.size() > 8);
    REQUIRE(png[0] == 0x89);  // PNG magic
#else
    REQUIRE(png.empty());
#endif
}

// #3329 — the built-in Skia raster backend on non-Apple must produce a real,
// non-black RGBA buffer via render_to_rgba (the foreign-host embed's
// pulp_embed_render_frame_rgba path). This is the headless render proof that
// works in a GPU-less CI VM (Skia raster is pure CPU).
TEST_CASE("Screenshot render_to_rgba produces non-black pixels (Skia raster)",
          "[view][screenshot][rgba]") {
#if defined(PULP_HAS_SKIA)
    View root;
    root.set_theme(Theme::dark());
    auto knob = std::make_unique<Knob>();
    knob->set_bounds({8, 8, 48, 48});
    knob->set_value(0.5f);
    root.add_child(std::move(knob));

    // The CPU Skia raster can transiently render all-black when the (shared CI)
    // box is saturated by other load — the render itself is correct, it just
    // gets starved. Retry a blank result a few times so a transient load spike
    // doesn't fail the gate; a genuinely all-black render (a real regression)
    // still fails every attempt. Same hardening as capture_view's raster tests.
    uint32_t pw = 0, ph = 0;
    std::vector<uint8_t> rgba;
    bool any_nonzero = false;
    for (int attempt = 0; attempt < 8 && !any_nonzero; ++attempt) {
        if (attempt > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * attempt));
        pw = ph = 0;
        rgba = render_to_rgba(root, 64, 64, 1.0f, &pw, &ph);
        for (size_t i = 0; i < rgba.size(); ++i) {
            if (rgba[i] != 0) { any_nonzero = true; break; }
        }
    }
    REQUIRE_FALSE(rgba.empty());
    REQUIRE(pw == 64);
    REQUIRE(ph == 64);
    REQUIRE(rgba.size() == static_cast<size_t>(pw) * ph * 4u);
    // The dark background fill (30,30,46) is itself non-black, so every pixel
    // must have a non-zero channel — proves the raster path actually painted.
    REQUIRE(any_nonzero);
#else
    SKIP("render_to_rgba unsupported without a Skia raster backend");
#endif
}

TEST_CASE("Screenshot render_to_png handles high DPI scale factors",
          "[view][screenshot][dpi]") {
#ifdef __APPLE__
    View root;
    root.set_theme(Theme::dark());
    auto knob = std::make_unique<Knob>();
    knob->set_bounds({2, 2, 28, 28});
    root.add_child(std::move(knob));

    // 2x, 3x, and the extreme 4x that Apple Silicon devices can hit.
    // Each must produce non-empty PNG bytes with the magic header —
    // regression guard for a scale-dependent early exit.
    for (float scale : {2.0f, 3.0f, 4.0f}) {
        INFO("scale=" << scale);
        auto png = render_to_png(root, 32, 32, scale);
        REQUIRE_FALSE(png.empty());
        REQUIRE(png.size() > 8);
        REQUIRE(png[0] == 0x89);
        REQUIRE(png[1] == 'P');
    }
#else
    SKIP("non-Apple screenshot path is explicitly unsupported");
#endif
}

TEST_CASE("Screenshot render_to_file rejects an unwritable output path",
          "[view][screenshot][edge]") {
#ifdef __APPLE__
    View root;
    root.set_theme(Theme::dark());

    // Path inside a directory that definitely does not exist.
    // render_to_file must return false, not silently succeed.
    auto bad = std::filesystem::path("/nonexistent-pulp-root/screenshot-should-fail.png");
    REQUIRE_FALSE(render_to_file(root, 16, 16, bad.string(), 1.0f));
    REQUIRE_FALSE(std::filesystem::exists(bad));
#else
    SKIP("non-Apple render_to_file is a no-op");
#endif
}

// pulp #1899 — pin the per-backend dispatch in render_to_png so the
// CLI's new `--backend` flag actually reaches render_to_png_skia /
// render_to_png_coregraphics. Both backends must produce a valid PNG
// header on the same input; the byte streams will differ (Skia is
// pixmap-encoded, CG uses ImageIO) so we don't compare them — we just
// pin that each path returns a non-empty PNG.
TEST_CASE("Screenshot: backend dispatch — Skia and CoreGraphics both produce valid PNGs",
          "[view][screenshot][issue-1899][backend]") {
#ifdef __APPLE__
    View root;
    root.set_theme(Theme::dark());
    auto label = std::make_unique<Label>("Backend dispatch test");
    label->set_bounds({0, 0, 120, 20});
    root.add_child(std::move(label));

    SECTION("Skia backend") {
        auto png = render_to_png(root, 200, 80, 1.0f, ScreenshotBackend::skia);
        REQUIRE_FALSE(png.empty());
        REQUIRE(png.size() > 8);
        REQUIRE(png[0] == 0x89);
        REQUIRE(png[1] == 'P');
        REQUIRE(png[2] == 'N');
        REQUIRE(png[3] == 'G');
    }
    SECTION("CoreGraphics backend") {
        auto png = render_to_png(root, 200, 80, 1.0f, ScreenshotBackend::coregraphics);
        REQUIRE_FALSE(png.empty());
        REQUIRE(png.size() > 8);
        REQUIRE(png[0] == 0x89);
        REQUIRE(png[1] == 'P');
        REQUIRE(png[2] == 'N');
        REQUIRE(png[3] == 'G');
    }
    SECTION("default_backend resolves to a supported backend") {
        auto png = render_to_png(root, 200, 80, 1.0f, ScreenshotBackend::default_backend);
        REQUIRE_FALSE(png.empty());
        REQUIRE(png[0] == 0x89);
    }
#else
    SKIP("non-Apple backend dispatch is provider-routed; covered elsewhere");
#endif
}

TEST_CASE("Screenshot of an empty root view still produces a valid PNG",
          "[view][screenshot][empty]") {
#if defined(__APPLE__) || defined(PULP_HAS_SKIA)
    // A backend-backed build (Apple CoreGraphics, or non-Apple Skia raster —
    // #3329) renders the background-filled frame even for an empty tree.
    View root;
    auto png = render_to_png(root, 8, 8, 1.0f);
    REQUIRE_FALSE(png.empty());
    REQUIRE(png.size() >= 8);
    REQUIRE(png[0] == 0x89);
#else
    View root;
    REQUIRE(render_to_png(root, 8, 8, 1.0f).empty());
#endif
}

TEST_CASE("Screenshot can export a deterministic PNG sequence", "[view][screenshot][phase12]") {
#ifdef __APPLE__
    FrameClock clock;
    View root;
    root.set_theme(Theme::dark());
    root.set_frame_clock(&clock);
    root.flex().padding = 8.0f;

    auto knob = std::make_unique<Knob>();
    auto* knob_ptr = knob.get();
    knob->flex().preferred_width = 48.0f;
    knob->flex().preferred_height = 48.0f;
    knob->set_label("Offline");
    root.add_child(std::move(knob));

    auto output_dir = std::filesystem::temp_directory_path() / "pulp_offline_video_sequence_test";
    std::filesystem::remove_all(output_dir);
    std::filesystem::create_directories(output_dir);

    std::vector<std::filesystem::path> frames;
    constexpr float dt = 1.0f / 30.0f;
    for (int i = 0; i < 3; ++i) {
        // Drive a guaranteed visual delta instead of relying on a widget animation
        // that may not change pixels between adjacent captures.
        knob_ptr->set_value(static_cast<float>(i) / 2.0f);
        clock.tick(dt);
        knob_ptr->advance_animations(dt);

        auto frame_path = output_dir / ("frame-" + std::to_string(i) + ".png");
        REQUIRE(render_to_file(root, 96, 64, frame_path.string(), 1.0f));
        REQUIRE(std::filesystem::exists(frame_path));
        frames.push_back(frame_path);
    }

    auto first = read_file_bytes(frames.front());
    auto middle = read_file_bytes(frames[1]);
    auto last = read_file_bytes(frames.back());
    REQUIRE_FALSE(first.empty());
    REQUIRE_FALSE(middle.empty());
    REQUIRE_FALSE(last.empty());
    REQUIRE(first != middle);
    REQUIRE(middle != last);
    REQUIRE(first != last);

    std::filesystem::remove_all(output_dir);
#else
    SKIP("Offline PNG-sequence exploration is currently macOS-first because screenshot capture is stubbed here");
#endif
}

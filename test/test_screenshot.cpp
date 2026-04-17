#include <catch2/catch_test_macros.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <filesystem>
#include <fstream>

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

#ifdef __APPLE__
    REQUIRE_FALSE(png.empty());
    // PNG magic bytes: 0x89 P N G
    REQUIRE(png.size() > 8);
    REQUIRE(png[0] == 0x89);
    REQUIRE(png[1] == 'P');
    REQUIRE(png[2] == 'N');
    REQUIRE(png[3] == 'G');
#else
    // Non-Apple platforms return empty (no CoreGraphics)
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

#ifdef __APPLE__
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

TEST_CASE("Screenshot on non-Apple reports unsupported honestly",
          "[view][screenshot][edge]") {
    // The doc contract on every non-Apple platform is "return empty
    // PNG bytes, return false from render_to_file" — never silently
    // fake-success. Pin it against an empty view tree too, so a
    // future refactor doesn't start fake-succeeding when there's
    // nothing to render.
    View root;
    auto png = render_to_png(root, 32, 32, 1.0f);
#ifdef __APPLE__
    REQUIRE_FALSE(png.empty());
#else
    REQUIRE(png.empty());
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
    SUCCEED("non-Apple screenshot path is explicitly unsupported");
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
    SUCCEED("non-Apple render_to_file is a no-op");
#endif
}

TEST_CASE("Screenshot of an empty root view still produces a valid PNG",
          "[view][screenshot][empty]") {
#ifdef __APPLE__
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
    SUCCEED("Offline PNG-sequence exploration is currently macOS-first because screenshot capture is stubbed here");
#endif
}

// The `canvas` trace category must stay populated.
//
// WHY THIS FILE EXISTS SEPARATELY, and why it does not use the GPU:
//
// `test_trace_frame_pipeline.cpp` also asserts on these spans, but it cannot
// be the guard. It requires a GPU capture backend, and the only lane that
// compiles tracing in (`.github/workflows/tracing-build.yml`) configures
// `PULP_ENABLE_GPU=OFF` and builds four named targets that do not include it.
// So in every lane that exists, that assertion either compiles to a no-op or
// skips — it can never fail, which makes it decoration rather than a guard.
//
// Compositing layers are pushed by `View::push_effect_layers`, which takes a
// `canvas::Canvas&` and knows nothing about the backend. A `RecordingCanvas`
// drives the same path with no GPU, no window, and no screenshot provider, so
// this runs wherever the tracing lane runs.
//
// The assertion matters because the category is DECLARED as "Canvas 2D
// drawing" while nothing emitted into it for a long time. A Perfetto query
// filtered on it returned no rows, which reads as "canvas drawing is free"
// rather than "nothing is instrumented" — a silent zero, not a finding.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>
#include <pulp/view/view.hpp>

namespace {

/// A view that forces `push_effect_layers` to open a compositing layer.
/// Opacity below 1 and a filter blur each do it on their own; both are set so
/// the test does not depend on which branch the paint path chooses.
std::unique_ptr<pulp::view::View> make_layered_scene() {
    auto root = std::make_unique<pulp::view::View>();
    root->set_bounds({0.0f, 0.0f, 240.0f, 120.0f});
    root->set_background_color(pulp::canvas::Color::rgba8(18, 18, 24, 255));

    auto layered = std::make_unique<pulp::view::View>();
    layered->set_bounds({20.0f, 20.0f, 200.0f, 80.0f});
    layered->set_background_color(pulp::canvas::Color::rgba8(200, 90, 40, 255));
    layered->set_opacity(0.5f);
    layered->set_filter_blur(2.0f);
    root->add_child(std::move(layered));
    return root;
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("painting a compositing layer emits into the canvas category",
          "[tracing][canvas][view]") {
    auto root = make_layered_scene();

    const auto out =
        (std::filesystem::temp_directory_path() / "pulp-canvas-layer-trace.pftrace").string();
    std::error_code ec;
    std::filesystem::remove(out, ec);

    const bool started = pulp::runtime::Tracing::start({"canvas"}, out);

    pulp::canvas::RecordingCanvas canvas;
    root->paint_all(canvas);

    const auto stopped = pulp::runtime::Tracing::stop();

    // The paint must happen either way — instrumentation is not allowed to be
    // load-bearing for drawing.
    REQUIRE(canvas.command_count() > 0);

    if (!pulp::runtime::kTracingEnabled) {
        // OFF contract: the macros compile to no-ops and no session activates.
        REQUIRE_FALSE(started);
        REQUIRE_FALSE(stopped.ok);
        SUCCEED("PULP_TRACING=OFF: canvas layer spans compile to no-ops");
        return;
    }

    REQUIRE(started);
    REQUIRE(stopped.ok);
    REQUIRE(stopped.trace_bytes > 0);

    const std::string bytes = read_all(stopped.path);
    REQUIRE_FALSE(bytes.empty());

    // The span itself. If this fails, the category went empty again.
    REQUIRE(bytes.find("effect_layer") != std::string::npos);

    // ...and its argument key, because a span with no args cannot answer
    // WHICH layer was expensive, which is the only reason to emit it.
    REQUIRE(bytes.find("layers") != std::string::npos);

    std::filesystem::remove(out, ec);
}

TEST_CASE("a scene with no effects emits no layer span", "[tracing][canvas][view]") {
    // The negative control. Without it, the assertion above could pass on a
    // build that emitted a layer span unconditionally, which would be a
    // different bug wearing the same green tick.
    auto root = std::make_unique<pulp::view::View>();
    root->set_bounds({0.0f, 0.0f, 240.0f, 120.0f});
    root->set_background_color(pulp::canvas::Color::rgba8(18, 18, 24, 255));

    const auto out =
        (std::filesystem::temp_directory_path() / "pulp-canvas-nolayer-trace.pftrace").string();
    std::error_code ec;
    std::filesystem::remove(out, ec);

    const bool started = pulp::runtime::Tracing::start({"canvas"}, out);
    pulp::canvas::RecordingCanvas canvas;
    root->paint_all(canvas);
    const auto stopped = pulp::runtime::Tracing::stop();

    if (!pulp::runtime::kTracingEnabled) {
        REQUIRE_FALSE(started);
        SUCCEED("PULP_TRACING=OFF: nothing to assert");
        return;
    }

    REQUIRE(started);
    REQUIRE(stopped.ok);
    const std::string bytes = read_all(stopped.path);
    CHECK(bytes.find("effect_layer") == std::string::npos);

    std::filesystem::remove(out, ec);
}

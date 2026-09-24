// Frame-pipeline tracing: drives real frames headlessly through the offscreen
// GPU frame path (render_to_png_gpu → HeadlessSurface → layout / paint / submit
// / present) with a Perfetto session active, then proves the pipeline emitted
// the expected spans into a flushed .pftrace.
//
// The frame pipeline runs on the UI / render thread — safe for Perfetto (unlike
// the live audio callback, which D1 proved is not RT-safe). The spans checked
// here (render "frame", layout "layout_children", canvas "paint", gpu
// "gpu_submit" / "gpu_present") are the frame-pipeline instrumentation.
//
// Two build contracts, one test:
//   * PULP_TRACING=ON  — a session activates and the flushed trace contains the
//     frame span, its `frame_index` debug arg, and at least one other pass span.
//   * PULP_TRACING=OFF — every span macro is a no-op and the session never
//     activates; the frame path still renders. Verified, not skipped.
// Without a GPU capture backend (no Dawn adapter / headers-only build) the test
// self-skips cleanly — a SKIP is never a false PASS.

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/trace.hpp>          // kTracingEnabled
#include <pulp/runtime/trace_session.hpp>  // Tracing::start / stop
#include <pulp/view/screenshot.hpp>        // render_to_png_gpu / has_gpu_capture
#include <pulp/view/view.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

namespace {

// A small view tree with opaque children so layout + paint do real work.
//
// One child carries opacity and a filter blur so the paint walks
// push_effect_layers / pop_effect_layers. Without it the scene pushes no
// compositing layer at all and the `canvas` category stays empty — which is
// indistinguishable from the instrumentation being absent, and is exactly the
// trap this test exists to close.
std::unique_ptr<pulp::view::View> make_scene() {
    auto root = std::make_unique<pulp::view::View>();
    root->set_background_color(pulp::canvas::Color::rgba8(18, 18, 24, 255));

    auto layered = std::make_unique<pulp::view::View>();
    layered->set_bounds({20.0f, 76.0f, 200.0f, 40.0f});
    layered->set_background_color(pulp::canvas::Color::rgba8(200, 90, 40, 255));
    layered->set_opacity(0.5f);
    layered->set_filter_blur(2.0f);
    root->add_child(std::move(layered));

    for (int i = 0; i < 3; ++i) {
        auto box = std::make_unique<pulp::view::View>();
        box->set_bounds({static_cast<float>(20 + i * 60), 20.0f, 48.0f, 48.0f});
        box->set_background_color(
            pulp::canvas::Color::rgba8(static_cast<uint8_t>(60 + i * 60), 140, 220, 255));
        root->add_child(std::move(box));
    }
    return root;
}

std::string read_all(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("frame pipeline emits Perfetto spans across a headless render",
          "[gpu][tracing][frame][view]") {
    if (!pulp::view::has_gpu_capture()) {
        SKIP("no GPU capture backend compiled in — frame-pipeline trace test skipped");
    }

    auto root = make_scene();
    constexpr uint32_t kW = 256, kH = 128;

    // Probe the GPU frame path before asserting anything: a build with the GPU
    // backend can still lack a usable Dawn adapter at runtime (CI VM, no device).
    if (pulp::view::render_to_png_gpu(*root, kW, kH, 1.0f).empty()) {
        SKIP("GPU frame path unavailable at runtime (no Dawn adapter) — skipped");
    }

    const auto out = (std::filesystem::temp_directory_path() /
                      "pulp-frame-pipeline-trace.pftrace")
                         .string();
    std::error_code ec;
    std::filesystem::remove(out, ec);

    const bool started =
        pulp::runtime::Tracing::start({"render", "layout", "canvas", "gpu"}, out);

    for (int i = 0; i < 8; ++i) {
        (void)pulp::view::render_to_png_gpu(*root, kW, kH, 1.0f);
    }

    const auto stopped = pulp::runtime::Tracing::stop();

    if (!pulp::runtime::kTracingEnabled) {
        // OFF contract: macros are no-ops, the session never activates, the
        // frame path still renders.
        REQUIRE_FALSE(started);
        REQUIRE_FALSE(stopped.ok);
        SUCCEED("PULP_TRACING=OFF: frame-pipeline spans compile to no-ops");
        return;
    }

    REQUIRE(started);
    REQUIRE(stopped.ok);
    REQUIRE_FALSE(stopped.path.empty());
    REQUIRE(stopped.trace_bytes > 0);

    const std::string bytes = read_all(stopped.path);
    REQUIRE_FALSE(bytes.empty());

    // The top-level frame span and its debug arg key (stored as debug.frame_index).
    REQUIRE(bytes.find("frame") != std::string::npos);
    REQUIRE(bytes.find("frame_index") != std::string::npos);

    // At least one further pass span from a different category must be present,
    // proving the frame span brackets real layout / paint / GPU work.
    const bool has_other_pass =
        bytes.find("layout_children") != std::string::npos ||
        bytes.find("paint") != std::string::npos ||
        bytes.find("gpu_submit") != std::string::npos ||
        bytes.find("gpu_present") != std::string::npos;
    REQUIRE(has_other_pass);

    // The `canvas` category is DECLARED as "Canvas 2D drawing"; for a long
    // time nothing emitted into it, so a query filtered on it returned no
    // rows — which reads as "canvas drawing is free" rather than "nothing is
    // instrumented". The scene above forces a compositing layer, so this is
    // the guard that the category stays populated.
    REQUIRE(bytes.find("effect_layer") != std::string::npos);

    // ...and that its argument keys survive, since a span with no args cannot
    // answer which layer was expensive.
    REQUIRE(bytes.find("layers") != std::string::npos);

    std::filesystem::remove(out, ec);
}

// Pins the no-alloc contract boundary around offscreen capture.
//
// View::paint_all opens a pulp::runtime::ScopedNoAlloc — "treat paint like the
// audio thread". Test binaries that link test/native_components/
// rt_intercept_test_support.cpp override the global operator new to ABORT on any
// allocation inside that scope. The paint path has never satisfied the contract
// on any platform: Skia's CPU device builds an SkPath for every rounded rect
// (SkBitmapDevice::drawRRect -> SkPath::RRect -> SkPathData::MakeNoCheck), and
// TextShaper::resolve_typeface builds the font-family fallback vector<string>.
//
// So an offscreen capture, which is a NON-real-time event by definition (one
// shot, no audio thread, no frame deadline), suspends the contract across the
// paint pass. That is what these cases pin, from both directions:
//
//   * live paint still runs UNDER the contract, so a genuine per-frame
//     allocation in widget code is still caught; and
//   * a capture runs with the contract SUSPENDED, so it completes instead of
//     aborting the process.
//
// The failure mode this guards against: the RT interceptor is linked per-BINARY,
// not per-test. A suite that links it to police one allocation-free
// Processor::process() also arms the abort for every paint that suite performs,
// so an editor suite that captures screenshots dies on the first rounded rect.
// Suspending the contract for captures — and only for captures — is what keeps
// both halves working in one binary.

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/screenshot.hpp>

#include <cstdint>

#if defined(__unix__) || defined(__APPLE__)
#include "native_components/rt_test_scope.hpp"
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

class NoOpCanvas final : public pulp::canvas::Canvas {
public:
    void save() override {}
    void restore() override {}
    void translate(float, float) override {}
    void scale(float, float) override {}
    void rotate(float) override {}
    void clip_rect(float, float, float, float) override {}
    void set_fill_color(pulp::canvas::Color) override {}
    void set_stroke_color(pulp::canvas::Color) override {}
    void set_line_width(float) override {}
    void set_line_cap(pulp::canvas::LineCap) override {}
    void set_line_join(pulp::canvas::LineJoin) override {}
    void fill_rect(float, float, float, float) override {}
    void stroke_rect(float, float, float, float) override {}
    void fill_rounded_rect(float, float, float, float, float) override {}
    void stroke_rounded_rect(float, float, float, float, float) override {}
    void fill_circle(float, float, float) override {}
    void stroke_circle(float, float, float) override {}
    void stroke_arc(float, float, float, float, float) override {}
    void stroke_line(float, float, float, float) override {}
    void set_font(const std::string&, float) override {}
    void set_text_align(pulp::canvas::TextAlign) override {}
    void fill_text(const std::string&, float, float) override {}
    float measure_text(const std::string&) override { return 0.0f; }
};

// Records what the no-alloc contract reported at the moment it was painted.
class ContractProbeView : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas&) override {
        painted = true;
        in_no_alloc_scope_during_paint = pulp::runtime::is_in_no_alloc_scope();
        guard_depth_during_paint = pulp::runtime::no_alloc_scope_depth();
    }

    bool painted = false;
    bool in_no_alloc_scope_during_paint = false;
    int guard_depth_during_paint = 0;
};

void require_capture_was_suspended(const ContractProbeView& root) {
    REQUIRE(root.painted);
    REQUIRE_FALSE(root.in_no_alloc_scope_during_paint);
#ifndef NDEBUG
    // The inner ScopedNoAlloc still exists; only its enforcement is suspended.
    REQUIRE(root.guard_depth_during_paint > 0);
#endif
}

}  // namespace

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("offscreen capture contract target has the allocator trap armed",
          "[view][rt-safety][issue-6344]") {
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        pulp::native_components::test::RtNoAllocScope no_alloc;
        void* allocation = ::operator new(64);
        ::operator delete(allocation);
        ::_exit(0);
    }

    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFSIGNALED(status));
    REQUIRE(WTERMSIG(status) == SIGABRT);
}
#endif

TEST_CASE("live paint runs under the no-alloc contract",
          "[view][rt-safety][issue-6344]") {
    ContractProbeView root;
    root.set_bounds({0, 0, 64, 64});

    // This canvas itself never allocates, so the only state under test is the
    // live paint contract rather than a recording backend's command buffer.
    NoOpCanvas canvas;
    root.paint_all(canvas);

    REQUIRE(root.painted);
#ifdef NDEBUG
    // Release: ScopedNoAlloc's body is #ifndef NDEBUG (scoped_no_alloc.hpp keeps
    // the SYMBOL so mixed-mode links stay ABI-stable, but the body is a no-op),
    // so there is no contract state to observe and nothing to assert about it.
    REQUIRE(root.guard_depth_during_paint == 0);
#else
    REQUIRE(root.guard_depth_during_paint > 0);
    // Live paint keeps the contract armed, so a real per-frame allocation in
    // widget code is still caught by the RT interceptor.
    REQUIRE(root.in_no_alloc_scope_during_paint);
#endif
}

#ifdef PULP_HAS_SKIA
TEST_CASE("offscreen Skia raster capture suspends the no-alloc contract",
          "[view][rt-safety][issue-6344]") {
    ContractProbeView root;
    root.set_bounds({0, 0, 64, 64});

    const auto png = pulp::view::render_to_png(root, 64, 64, 1.0f,
                                               pulp::view::ScreenshotBackend::skia);
    require_capture_was_suspended(root);
    REQUIRE_FALSE(png.empty());
}
#endif

#ifdef __APPLE__
TEST_CASE("offscreen CoreGraphics capture suspends the no-alloc contract",
          "[view][rt-safety][issue-6344]") {
    ContractProbeView root;
    const auto png = pulp::view::render_to_png(
        root, 64, 64, 1.0f, pulp::view::ScreenshotBackend::coregraphics);
    require_capture_was_suspended(root);
    REQUIRE_FALSE(png.empty());
}
#endif

#ifdef PULP_HAS_SKIA
TEST_CASE("offscreen raw RGBA capture suspends the no-alloc contract",
          "[view][rt-safety][issue-6344]") {
    ContractProbeView root;
    uint32_t width = 0;
    uint32_t height = 0;
    const auto rgba =
        pulp::view::render_to_rgba(root, 64, 64, 1.0f, &width, &height);
    require_capture_was_suspended(root);
    REQUIRE_FALSE(rgba.empty());
    REQUIRE(width == 64);
    REQUIRE(height == 64);
}
#endif

TEST_CASE("offscreen GPU capture suspends the no-alloc contract",
          "[view][rt-safety][issue-6344][gpu]") {
    if (!pulp::view::has_gpu_capture()) {
        SKIP("GPU capture is not compiled into this build");
    }

    ContractProbeView root;
    const auto png = pulp::view::render_to_png_gpu(root, 64, 64, 1.0f);
    if (png.empty() && !root.painted) {
        SKIP("GPU capture is compiled in but no runtime adapter is available");
    }
    require_capture_was_suspended(root);
    REQUIRE_FALSE(png.empty());
}

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
#include <pulp/view/view.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/canvas/recording_canvas.hpp>

#include <cstdint>

namespace {

// Records what the no-alloc contract reported at the moment it was painted.
class ContractProbeView : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas& canvas) override {
        painted = true;
        in_no_alloc_scope_during_paint = pulp::runtime::is_in_no_alloc_scope();
        guard_depth_during_paint = pulp::runtime::no_alloc_scope_depth();
        View::paint(canvas);
    }

    bool painted = false;
    bool in_no_alloc_scope_during_paint = false;
    int guard_depth_during_paint = 0;
};

}  // namespace

TEST_CASE("live paint runs under the no-alloc contract",
          "[view][rt-safety][issue-6344]") {
    ContractProbeView root;
    root.set_bounds({0, 0, 64, 64});

    // Paint straight into a canvas, the way a live window host does — no
    // capture wrapper in the call chain.
    pulp::canvas::RecordingCanvas canvas;
    root.paint_all(canvas);

    REQUIRE(root.painted);
    REQUIRE(root.guard_depth_during_paint > 0);
    // Live paint keeps the contract armed, so a real per-frame allocation in
    // widget code is still caught by the RT interceptor.
    REQUIRE(root.in_no_alloc_scope_during_paint);
}

TEST_CASE("offscreen capture suspends the no-alloc contract",
          "[view][rt-safety][issue-6344]") {
    ContractProbeView root;
    root.set_bounds({0, 0, 64, 64});

    const auto png = pulp::view::render_to_png(root, 64, 64, 1.0f,
                                               pulp::view::ScreenshotBackend::skia);

    // The capture must have actually painted us — otherwise the assertion below
    // would pass vacuously on a build where the backend silently no-ops.
    REQUIRE(root.painted);

    // The ScopedNoAlloc guard is still ON THE STACK during the capture (paint_all
    // opens it unconditionally) ...
    REQUIRE(root.guard_depth_during_paint > 0);
    // ... but the contract reports SUSPENDED, which is what stops the RT
    // interceptor from aborting the capture.
    REQUIRE_FALSE(root.in_no_alloc_scope_during_paint);

    REQUIRE_FALSE(png.empty());
}

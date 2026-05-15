#pragma once

/// @file mac_window_harness.hpp
/// Mac-only Catch2 test harness — headless NSWindow + CAMetalLayer fixture
/// (issue #2001).
///
/// The harness reuses the production `WindowHost` GPU path and exposes a
/// minimal API so tests can:
///
///   1. construct a hidden GPU window without orderFront / activation,
///   2. synthesize AppKit mouse events against its real content view,
///   3. read deterministic back-buffer PNG bytes.
///
/// The harness lives in `test/` only — never installed into the SDK. The
/// production seam it depends on (`WindowHost::capture_back_buffer_png`)
/// is the only public API addition.
///
/// First customer (Phase B-1, this PR): a smoke test that proves the
/// harness can construct a hidden GPU window and the back-buffer capture
/// path returns non-empty bytes. Phase B-2 will migrate one PR-#1984
/// invariant (e.g. set_design_viewport overlay-inside-transform) to the
/// harness as the first "real" consumer.
///
/// Plan: planning/2026-05-14-mac-platform-test-harness.md (planning
/// submodule).

#include <pulp/view/input_events.hpp>
#include <pulp/view/window_host.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::view { class View; }

namespace pulp::test::mac {

/// One synthetic mouse event. Coordinates use Pulp logical pixels with
/// top-left origin (matching `View::on_mouse_event` semantics). The
/// harness handles the Cocoa bottom-left conversion internally.
struct SimulatedMouse {
    enum class Phase { down, up, move, drag, scroll };

    Phase phase = Phase::move;
    float x = 0.0f;
    float y = 0.0f;
    pulp::view::MouseButton button = pulp::view::MouseButton::left;
    uint16_t modifiers = 0;
    int click_count = 1;
    float scroll_delta_x = 0.0f;
    float scroll_delta_y = 0.0f;
};

/// Construct a hidden GPU-backed NSWindow + CAMetalLayer host suitable for
/// unit tests. Forces `options.use_gpu = true` and
/// `options.initially_hidden = true` so callers cannot accidentally pop a
/// real window during a test run.
///
/// Returns nullptr on any failure (host construction, native handles
/// missing, gpu_surface unavailable). Never throws.
///
/// Ownership: the caller owns the returned host. The `root` view must
/// outlive the host, matching `WindowHost::create()` semantics.
std::unique_ptr<pulp::view::WindowHost>
make_test_window(pulp::view::View& root,
                 pulp::view::WindowOptions options = {});

/// Synthesize a single AppKit mouse event against the host's content view
/// and drain the main run-loop once so deferred click handlers (which
/// `PulpView::mouseUp:` posts via `dispatch_async`) settle before the
/// caller asserts or captures.
///
/// Returns false on null handles, zero content size, or unsupported
/// phase. Never throws.
bool simulate_mouse(pulp::view::WindowHost& host, const SimulatedMouse& event);

/// Wrapper over `WindowHost::capture_back_buffer_png` that drains the
/// main queue once first so any pending render / deferred state mutations
/// are ordered before the readback. Returns the host's PNG bytes (may be
/// empty on capture failure — the harness does not raise).
std::vector<uint8_t> capture_back_buffer_png(pulp::view::WindowHost& host);

} // namespace pulp::test::mac

// Headless screenshot capture helper for StandaloneApp.
//
// Factored out of standalone.cpp so the capture state machine (frame
// counter + one-shot guard + capture/write/close composition) is
// unit-testable without a real WindowHost. The runtime wiring composes
// this with the existing idle callback (inspector + scripted_ui + settings
// poll) inside StandaloneApp::run_with_editor.
//
// Lifecycle (called once per idle tick):
//   frames 0 .. delay-1  → increment counter, return.
//   frame delay          → invoke capture_fn(), write bytes to path,
//                          invoke close_fn(), set the one-shot guard.
//   frame delay+1 ..     → guarded no-op.
//
// All side effects (PNG bytes, file writes, window close) are injected
// as std::function so tests can substitute deterministic stand-ins.

#pragma once

#include <cstdint>
#include <functional>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace pulp::format::detail {

/// Apply one `screenshot_actions` entry. Grammar is deliberately tiny and
/// app-agnostic: the SDK must be able to drive a plugin it knows nothing about.
///
///   command:<id>        invoke a Processor command by its declared ID
///   click:<view-id>     simulate a click on the View with that id()
///   scroll:<id>=<frac>  scroll a ScrollView to a fraction of its range
///
/// A miss is reported, never swallowed. A capture that quietly failed to open
/// the panel it was told to open is indistinguishable from a passing capture of
/// a broken one, which is the exact confusion this whole path exists to end.
void apply_screenshot_action(const std::string& action,
                             Processor* processor,
                             view::View* root);

struct ScreenshotCapture {
    int delay = 30;
    std::string path;
    std::function<std::vector<uint8_t>()> capture_fn;
    std::function<void()> close_fn;
    std::function<void(const std::string&)> on_error;

    // Driving the surface before the shutter. A screenshot of the launch state
    // can only ever photograph the launch state, so a panel reached by a click
    // — a settings modal, a preset browser, a dialog — was simply not
    // photographable, and the surfaces most worth reviewing are exactly those.
    // Actions run once, `settle_frames` before the capture, so the tree has
    // frames to lay out and animate before the shutter.
    std::function<void()> actions_fn;
    // Written at the same frame as the PNG, so the tree and the pixels
    // describe the same instant.
    std::function<void()> layout_fn;
    int settle_frames = 0;
    std::shared_ptr<bool> acted = std::make_shared<bool>(false);

    // Heap-allocated state — the capture is wrapped in a std::function and
    // copied into the WindowHost's idle callback, so two layers of copies
    // must observe the same counter.
    std::shared_ptr<int> frame = std::make_shared<int>(0);
    std::shared_ptr<bool> captured = std::make_shared<bool>(false);

    void operator()() {
        if (*captured) return;
        ++(*frame);
        // Act first, then let the surface settle. Running the actions on the
        // capture frame itself would photograph a tree that has not laid out.
        if (!*acted && actions_fn && *frame >= delay - settle_frames) {
            *acted = true;
            actions_fn();
        }
        if (*frame < delay) return;
        *captured = true;
        if (layout_fn) layout_fn();
        auto bytes = capture_fn ? capture_fn() : std::vector<uint8_t>{};
        if (bytes.empty()) {
            if (on_error) on_error("capture_png returned empty bytes");
        } else if (path.empty()) {
            if (on_error) on_error("screenshot path is empty");
        } else {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
            if (!out && on_error) on_error("failed to write screenshot");
        }
        if (close_fn) close_fn();
    }
};

}  // namespace pulp::format::detail

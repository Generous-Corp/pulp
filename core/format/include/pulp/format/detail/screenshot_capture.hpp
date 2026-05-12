// pulp #468 — headless screenshot capture helper for StandaloneApp.
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

struct ScreenshotCapture {
    int delay = 30;
    std::string path;
    std::function<std::vector<uint8_t>()> capture_fn;
    std::function<void()> close_fn;
    std::function<void(const std::string&)> on_error;

    // Heap-allocated state — the capture is wrapped in a std::function and
    // copied into the WindowHost's idle callback, so two layers of copies
    // must observe the same counter.
    std::shared_ptr<int> frame = std::make_shared<int>(0);
    std::shared_ptr<bool> captured = std::make_shared<bool>(false);

    void operator()() {
        if (*captured) return;
        ++(*frame);
        if (*frame < delay) return;
        *captured = true;
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

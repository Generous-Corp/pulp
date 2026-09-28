// pulp #468 — unit test for headless screenshot capture state machine.
//
// Verifies the delay → capture → write → close lifecycle without a real
// WindowHost. The runtime composes this with the inspector / scripted_ui
// / settings idle callback chain inside StandaloneApp::run_with_editor;
// here we exercise only the capture helper in isolation.

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/delayed_action.hpp>
#include <pulp/format/detail/frame_time_summary.hpp>
#include <pulp/format/detail/screenshot_capture.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <vector>

using pulp::format::detail::ScreenshotCapture;
using pulp::format::detail::DelayedAction;

namespace {

struct CaptureHarness {
    int capture_calls = 0;
    int close_calls = 0;
    std::vector<std::string> errors;
    std::vector<uint8_t> bytes_to_return = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    ScreenshotCapture make(const std::string& path, int delay) {
        ScreenshotCapture cap;
        cap.delay = delay;
        cap.path = path;
        cap.capture_fn = [this] { ++capture_calls; return bytes_to_return; };
        cap.close_fn   = [this] { ++close_calls; };
        cap.on_error   = [this](const std::string& msg) { errors.push_back(msg); };
        return cap;
    }
};

std::string tmp_screenshot_path(const char* suffix) {
    auto dir = std::filesystem::temp_directory_path();
    return (dir / (std::string("pulp_test_screenshot_") + suffix + ".png")).string();
}

std::vector<uint8_t> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    std::vector<uint8_t> out(size);
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    return out;
}

}  // namespace

TEST_CASE("ScreenshotCapture waits delay frames before capturing", "[issue-468][screenshot]") {
    CaptureHarness h;
    auto path = tmp_screenshot_path("delay");
    std::filesystem::remove(path);
    auto cap = h.make(path, 5);

    // Frames 1..4: no capture, no close, file does not yet exist.
    for (int i = 0; i < 4; ++i) {
        cap();
        REQUIRE(h.capture_calls == 0);
        REQUIRE(h.close_calls == 0);
        REQUIRE_FALSE(std::filesystem::exists(path));
    }

    // Frame 5: capture, write, close fire exactly once.
    cap();
    REQUIRE(h.capture_calls == 1);
    REQUIRE(h.close_calls == 1);
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(read_file_bytes(path) == h.bytes_to_return);

    std::filesystem::remove(path);
}

TEST_CASE("ScreenshotCapture is one-shot — extra ticks no-op", "[issue-468][screenshot]") {
    CaptureHarness h;
    auto path = tmp_screenshot_path("oneshot");
    std::filesystem::remove(path);
    auto cap = h.make(path, 2);

    for (int i = 0; i < 10; ++i) cap();

    REQUIRE(h.capture_calls == 1);
    REQUIRE(h.close_calls == 1);
    std::filesystem::remove(path);
}

TEST_CASE("ScreenshotCapture reports error on empty capture bytes", "[issue-468][screenshot]") {
    CaptureHarness h;
    h.bytes_to_return.clear();
    auto path = tmp_screenshot_path("empty");
    std::filesystem::remove(path);
    auto cap = h.make(path, 1);

    cap();
    REQUIRE(h.capture_calls == 1);
    REQUIRE(h.errors.size() == 1);
    REQUIRE(h.errors[0].find("empty") != std::string::npos);
    // close_fn always fires even on error so the app exits deterministically.
    REQUIRE(h.close_calls == 1);
    // No file written when bytes are empty.
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("ScreenshotCapture survives copies — shared state advances together",
          "[issue-468][screenshot]") {
    // The runtime wraps the capture into a std::function and copies it
    // into WindowHost's idle callback. With value-typed state, the copy
    // would have its OWN frame counter and never trigger. Shared-state
    // pointers ensure both copies advance the same counter.
    CaptureHarness h;
    auto path = tmp_screenshot_path("copy");
    std::filesystem::remove(path);
    auto cap = h.make(path, 3);

    std::function<void()> wrapped = cap;  // copy
    wrapped();  // frame 1
    wrapped();  // frame 2
    REQUIRE(h.capture_calls == 0);
    cap();      // frame 3 via the original — same shared state
    REQUIRE(h.capture_calls == 1);
    REQUIRE(h.close_calls == 1);
    std::filesystem::remove(path);
}

TEST_CASE("ScreenshotCapture default delay (30) honored when caller leaves field unset",
          "[issue-468][screenshot]") {
    // The runtime defaults to 30 frames; verify the helper's own default
    // matches (so an unset config doesn't fire on the first tick).
    ScreenshotCapture cap;
    REQUIRE(cap.delay == 30);
}

TEST_CASE("ScreenshotCapture handles missing capture callback as empty bytes",
          "[issue-468][screenshot]") {
    CaptureHarness h;
    auto path = tmp_screenshot_path("missing_capture");
    std::filesystem::remove(path);
    auto cap = h.make(path, 1);
    cap.capture_fn = {};

    cap();
    REQUIRE(h.capture_calls == 0);
    REQUIRE(h.close_calls == 1);
    REQUIRE(h.errors.size() == 1);
    REQUIRE(h.errors[0].find("empty") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(path));
    REQUIRE(*cap.captured);

    cap();
    REQUIRE(h.close_calls == 1);
}

TEST_CASE("ScreenshotCapture reports empty screenshot path before writing",
          "[issue-468][screenshot]") {
    CaptureHarness h;
    auto cap = h.make("", 1);

    cap();
    REQUIRE(h.capture_calls == 1);
    REQUIRE(h.close_calls == 1);
    REQUIRE(h.errors.size() == 1);
    REQUIRE(h.errors[0].find("path is empty") != std::string::npos);
    REQUIRE(*cap.captured);
}

TEST_CASE("ScreenshotCapture tolerates missing error callback for bad inputs",
          "[issue-468][screenshot]") {
    CaptureHarness h;
    auto cap = h.make("", 1);
    cap.on_error = {};

    cap();
    REQUIRE(h.capture_calls == 1);
    REQUIRE(h.close_calls == 1);
    REQUIRE(h.errors.empty());
    REQUIRE(*cap.captured);
}

TEST_CASE("ScreenshotCapture reports failed file writes and still closes",
          "[issue-468][screenshot]") {
    CaptureHarness h;
    auto missing_dir = std::filesystem::temp_directory_path() /
                       "pulp_test_screenshot_missing_dir" /
                       "capture.png";
    std::filesystem::remove_all(missing_dir.parent_path());
    auto cap = h.make(missing_dir.string(), 1);

    cap();
    REQUIRE(h.capture_calls == 1);
    REQUIRE(h.close_calls == 1);
    REQUIRE(h.errors.size() == 1);
    REQUIRE(h.errors[0].find("failed to write") != std::string::npos);
    REQUIRE_FALSE(std::filesystem::exists(missing_dir));
    REQUIRE(*cap.captured);
}

TEST_CASE("ScreenshotCapture skips write error reporting when no handler is set",
          "[issue-468][screenshot]") {
    CaptureHarness h;
    auto missing_dir = std::filesystem::temp_directory_path() /
                       "pulp_test_screenshot_missing_dir_no_error" /
                       "capture.png";
    std::filesystem::remove_all(missing_dir.parent_path());
    auto cap = h.make(missing_dir.string(), 1);
    cap.on_error = {};

    cap();
    REQUIRE(h.capture_calls == 1);
    REQUIRE(h.close_calls == 1);
    REQUIRE(h.errors.empty());
    REQUIRE_FALSE(std::filesystem::exists(missing_dir));
}

TEST_CASE("ScreenshotCapture permits missing close callback after a successful write",
          "[issue-468][screenshot]") {
    CaptureHarness h;
    auto path = tmp_screenshot_path("missing_close");
    std::filesystem::remove(path);
    auto cap = h.make(path, 1);
    cap.close_fn = {};

    cap();
    REQUIRE(h.capture_calls == 1);
    REQUIRE(h.close_calls == 0);
    REQUIRE(h.errors.empty());
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(read_file_bytes(path) == h.bytes_to_return);
    REQUIRE(*cap.captured);

    cap();
    REQUIRE(h.capture_calls == 1);
    std::filesystem::remove(path);
}

TEST_CASE("ScreenshotCapture captures immediately for non-positive delays",
          "[issue-468][screenshot]") {
    CaptureHarness zero_delay;
    auto zero_path = tmp_screenshot_path("zero_delay");
    std::filesystem::remove(zero_path);
    auto zero = zero_delay.make(zero_path, 0);

    zero();
    REQUIRE(zero_delay.capture_calls == 1);
    REQUIRE(zero_delay.close_calls == 1);
    REQUIRE(*zero.frame == 1);
    REQUIRE(read_file_bytes(zero_path) == zero_delay.bytes_to_return);

    CaptureHarness negative_delay;
    auto negative_path = tmp_screenshot_path("negative_delay");
    std::filesystem::remove(negative_path);
    auto negative = negative_delay.make(negative_path, -4);

    negative();
    REQUIRE(negative_delay.capture_calls == 1);
    REQUIRE(negative_delay.close_calls == 1);
    REQUIRE(*negative.frame == 1);
    REQUIRE(read_file_bytes(negative_path) == negative_delay.bytes_to_return);

    std::filesystem::remove(zero_path);
    std::filesystem::remove(negative_path);
}

TEST_CASE("DelayedAction waits delay frames before firing side effect",
          "[issue-468][screenshot][audio-inspector]") {
    int actions = 0;
    int closes = 0;
    DelayedAction action;
    action.delay = 4;
    action.action_fn = [&] { ++actions; };
    action.close_fn = [&] { ++closes; };

    for (int i = 0; i < 3; ++i) {
        action();
        REQUIRE(actions == 0);
        REQUIRE(closes == 0);
    }

    action();
    REQUIRE(actions == 1);
    REQUIRE(closes == 1);

    action();
    REQUIRE(actions == 1);
    REQUIRE(closes == 1);
}

TEST_CASE("DelayedAction survives copies and shares the frame counter",
          "[issue-468][screenshot][audio-inspector]") {
    int actions = 0;
    int closes = 0;
    DelayedAction action;
    action.delay = 3;
    action.action_fn = [&] { ++actions; };
    action.close_fn = [&] { ++closes; };

    std::function<void()> wrapped = action;
    wrapped();
    wrapped();
    REQUIRE(actions == 0);

    action();
    REQUIRE(actions == 1);
    REQUIRE(closes == 1);
}

TEST_CASE("DelayedAction closes even when no action is installed",
          "[issue-468][screenshot][audio-inspector]") {
    int closes = 0;
    DelayedAction action;
    action.delay = 1;
    action.close_fn = [&] { ++closes; };

    action();
    REQUIRE(closes == 1);
    REQUIRE(*action.captured);
}

// ── Frame-time summary for the headless screenshot run ──────────────────────

using pulp::format::detail::format_frame_time_summary;
using pulp::format::detail::summarize_frame_times;

TEST_CASE("Frame-time summary uses nearest-rank percentiles and strict budgets",
          "[screenshot][frame-time]") {
    // 1..100 ms: nearest-rank p50 is the 50th sample, p95 the 95th.
    std::vector<double> samples;
    for (int i = 100; i >= 1; --i)  // unsorted input on purpose
        samples.push_back(static_cast<double>(i));
    const auto s = summarize_frame_times(samples);
    REQUIRE(s.frames == 100);
    REQUIRE(s.p50_ms == 50.0);
    REQUIRE(s.p95_ms == 95.0);
    REQUIRE(s.max_ms == 100.0);
    // Strictly over 16.67 ms: 17..100; strictly over 33.33 ms: 34..100.
    REQUIRE(s.over_16ms == 84);
    REQUIRE(s.over_33ms == 67);
}

TEST_CASE("Frame-time summary counts a frame exactly on budget as within it",
          "[screenshot][frame-time]") {
    const auto s = summarize_frame_times({1000.0 / 60.0, 1000.0 / 30.0, 16.8, 33.4});
    REQUIRE(s.over_16ms == 3);  // 33.33, 16.8, 33.4
    REQUIRE(s.over_33ms == 1);  // 33.4
}

TEST_CASE("Frame-time summary drops unusable samples and handles no frames",
          "[screenshot][frame-time]") {
    const auto empty = summarize_frame_times({});
    REQUIRE(empty.frames == 0);
    REQUIRE(format_frame_time_summary(empty) ==
            "frame_ms frames=0 p50=0.00 p95=0.00 max=0.00 over16=0 over33=0");

    const auto s = summarize_frame_times({-1.0, std::nan(""), 4.0, INFINITY});
    REQUIRE(s.frames == 1);
    REQUIRE(s.max_ms == 4.0);
}

TEST_CASE("Frame-time summary line keeps its greppable shape",
          "[screenshot][frame-time]") {
    std::vector<double> samples(59, 2.04);
    samples.push_back(40.0);
    const auto line = format_frame_time_summary(summarize_frame_times(samples));
    REQUIRE(line == "frame_ms frames=60 p50=2.04 p95=2.04 max=40.00 over16=1 over33=1");
}

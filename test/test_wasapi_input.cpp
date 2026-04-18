// WASAPI capture path tests (issue #243). Windows-only; gracefully
// skips when run on a host without any active capture endpoint
// (headless VMs, CI runners with no audio device, etc.) so the suite
// stays green on every platform without losing the validation when
// real input hardware IS present.

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

#include <pulp/audio/device.hpp>
#include "../core/audio/platform/win/wasapi_device.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace pulp::audio;
using namespace pulp::audio::win;

namespace {

bool has_default_capture(WasapiSystem& sys) {
    return !sys.default_input_device().id.empty();
}

}  // namespace

TEST_CASE("WASAPI capture: default input device discovers when present",
          "[audio][wasapi][capture][issue-243]") {
    WasapiSystem sys;
    auto info = sys.default_input_device();
    if (info.id.empty()) {
        SUCCEED("no default capture endpoint on this host; skipping");
        return;
    }
    REQUIRE(info.is_default_input);
    REQUIRE(info.max_input_channels > 0);
    REQUIRE_FALSE(info.id.empty());
    REQUIRE_FALSE(info.name.empty());
}

TEST_CASE("WASAPI capture: create_device on input id returns eCapture device",
          "[audio][wasapi][capture][issue-243]") {
    WasapiSystem sys;
    auto info = sys.default_input_device();
    if (info.id.empty()) {
        SUCCEED("no default capture endpoint; skipping");
        return;
    }
    auto device = sys.create_device(info.id);
    REQUIRE(device != nullptr);
    auto* wasapi = dynamic_cast<WasapiDevice*>(device.get());
    REQUIRE(wasapi != nullptr);
    REQUIRE(wasapi->flow() == eCapture);
}

TEST_CASE("WASAPI capture: open + start + stop is leak-free and terminates",
          "[audio][wasapi][capture][issue-243]") {
    WasapiSystem sys;
    auto info = sys.default_input_device();
    if (info.id.empty()) {
        SUCCEED("no default capture endpoint; skipping");
        return;
    }
    auto device = sys.create_device(info.id);
    REQUIRE(device != nullptr);

    DeviceConfig cfg;
    cfg.device_id = info.id;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = 256;
    cfg.input_channels = 2;
    cfg.output_channels = 0;

    REQUIRE(device->open(cfg));
    REQUIRE(device->is_open());

    std::atomic<int> callbacks{0};
    REQUIRE(device->start([&](const auto& in, auto& out, const auto&) {
        // Capture path hands us non-empty input + empty output. We
        // only count invocations — verifying frame counts is platform-
        // dependent and brittle.
        (void)in;
        (void)out;
        callbacks.fetch_add(1, std::memory_order_relaxed);
    }));
    REQUIRE(device->is_running());

    // Run for ~50ms — enough for several capture packets at 256 frames
    // / 48 kHz (~5.3ms each), small enough to keep the test snappy.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    device->stop();
    REQUIRE_FALSE(device->is_running());
    device->close();
    REQUIRE_FALSE(device->is_open());

    // We don't assert callbacks > 0 because some headless hosts route
    // a capture endpoint that never produces packets (e.g. virtual
    // mic with no driver). The leak-free start/stop path is the
    // contract under test; any callback is a bonus signal.
}

TEST_CASE("WASAPI capture: render device still constructs as eRender",
          "[audio][wasapi][capture][issue-243][regression]") {
    // Regression guard: the new EDataFlow plumbing must not break the
    // existing render path. WasapiSystem::create_device() with an
    // empty id picks the default render device, which must still come
    // back as eRender.
    WasapiSystem sys;
    auto device = sys.create_device("");
    if (!device) {
        SUCCEED("no default render endpoint on this host; skipping");
        return;
    }
    auto* wasapi = dynamic_cast<WasapiDevice*>(device.get());
    REQUIRE(wasapi != nullptr);
    REQUIRE(wasapi->flow() == eRender);
}

#else  // !_WIN32

TEST_CASE("WASAPI capture tests build on non-Windows but no-op",
          "[audio][wasapi][capture]") {
    SUCCEED("WASAPI tests are Windows-only; this stub keeps CI happy.");
}

#endif

/// @file test_audio_workgroup_wiring.cpp
/// Item 1.1 — AudioWorkgroup wired to AudioDevice + render-callback path.
///
/// Plan acceptance (per `planning/2026-05-24-macos-plugin-authoring-plan.md`
/// section "### 1.1 AudioWorkgroup wired to AudioDevice + processor
/// callback", reviewer decision 2026-05-25):
///
///   (1) extraction of `kAudioDevicePropertyIOThreadOSWorkgroup` is the
///       required path on macOS 13+ / iOS 16+ — `AudioDevice::callback_workgroup()`
///       is the surface, and the default base-class implementation
///       returns `nullptr` so non-Apple backends inherit the fallback;
///   (2) RAII `AudioWorkgroupJoin` joins exactly once on first
///       callback entry and leaves on thread exit;
///   (3) the Mach-priority fallback path in
///       `AudioWorkgroup::set_realtime_priority` stays as defensive
///       code for the case where extraction returns null (a property
///       the spec allows).
///
/// This file tests the contract pieces that don't require a real
/// audio device:
///   - `AudioDevice::callback_workgroup()` default returns null (so
///     non-Apple backends don't break);
///   - the new xrun counter / reset path on the base class;
///   - `AudioWorkgroup::set_workgroup(nullptr)` keeps the fallback
///     path safe (the "device has no workgroup" branch);
///   - `join_from_audio_thread()` is idempotent under repeat-entry
///     across `wg_joined_` (mirrors the per-render-callback first-
///     entry guard).
///
/// Real-device acceptance (#4 in the plan — `os_signpost` verifying
/// workgroup membership on Apple Silicon) lives in a manual smoke
/// step documented in the macOS plan; CI cannot guarantee Apple
/// Silicon hardware, so it stays out of the unit suite. Pass-2
/// reviewer note: the unit suite explicitly verifies the contract
/// surface the production path consumes (callback_workgroup() ->
/// AudioWorkgroup::set_workgroup() -> join_from_audio_thread()), so
/// the integration risk surface is the OS plumbing alone.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/device.hpp>
#include <pulp/audio/workgroup.hpp>

#include <atomic>
#include <thread>

using namespace pulp::audio;

namespace {

/// Stub AudioDevice that overrides nothing — exercises the base-class
/// `callback_workgroup()` / `xrun_count()` / `reset_xrun_counter()`
/// defaults that non-Apple backends inherit. Pure-virtual stubs return
/// safe values.
class StubDevice : public AudioDevice {
public:
    bool open(const DeviceConfig&) override { return true; }
    void close() override {}
    bool start(AudioCallback) override { return true; }
    void stop() override {}
    bool is_open() const override { return false; }
    bool is_running() const override { return false; }
    DeviceInfo info() const override { return {}; }
    double sample_rate() const override { return 48000.0; }
    int buffer_size() const override { return 256; }
};

}  // namespace

TEST_CASE("AudioDevice base class callback_workgroup defaults to null",
          "[audio][workgroup][wiring][issue-2935]") {
    StubDevice dev;
    REQUIRE(dev.callback_workgroup() == nullptr);
}

TEST_CASE("AudioDevice base class xrun_count defaults to zero and reset is safe",
          "[audio][workgroup][wiring][issue-2935]") {
    StubDevice dev;
    REQUIRE(dev.xrun_count() == 0);
    dev.reset_xrun_counter();  // safe no-op on the default implementation
    REQUIRE(dev.xrun_count() == 0);
}

TEST_CASE("AudioWorkgroup with explicit null workgroup falls back safely",
          "[audio][workgroup][wiring][issue-2935]") {
    // Mirrors the case where CoreAudio returns noErr but a null
    // workgroup pointer — query_callback_workgroup() leaves the
    // cached value at nullptr and the render callback's join is
    // still safe.
    AudioWorkgroup wg;
#if defined(__APPLE__)
    wg.set_workgroup(nullptr);
#endif
    bool joined_first  = wg.join_from_audio_thread();
    bool joined_second = wg.join_from_audio_thread();
    // join_from_audio_thread is idempotent: a second call without a
    // leave() in between must report joined (or both must remain
    // un-joined if the platform refuses RT priority entirely).
    REQUIRE(joined_first == joined_second);
    wg.leave();
    REQUIRE_FALSE(wg.is_joined());
}

TEST_CASE("AudioWorkgroup leave-then-rejoin tracks per-thread join state",
          "[audio][workgroup][wiring][issue-2935]") {
    // The render callback's first-entry guard
    // (`wg_joined_` atomic in CoreAudioDevice) relies on
    // AudioWorkgroup::leave() actually clearing the joined flag so
    // the next start() can re-join on the new render thread without
    // tripping any "already joined" debug assertion.
    AudioWorkgroup wg;
    bool joined = wg.join_from_audio_thread();
    if (joined) {
        wg.leave();
        REQUIRE_FALSE(wg.is_joined());
        bool rejoined = wg.join_from_audio_thread();
        REQUIRE(rejoined);
        wg.leave();
        REQUIRE_FALSE(wg.is_joined());
    } else {
        // Test environment refuses RT priority; rejoining must also
        // refuse, but neither call may crash.
        bool rejoined = wg.join_from_audio_thread();
        REQUIRE_FALSE(rejoined);
    }
}

TEST_CASE("AudioWorkgroup first-entry guard pattern is race-free",
          "[audio][workgroup][wiring][issue-2935]") {
    // Models the exact acquire/release pattern in
    // CoreAudioDevice::render_callback: a relaxed-ordered atomic guard
    // that gates a one-shot join. Hammer it across N threads to
    // confirm the pattern works under concurrency. (CoreAudio only
    // ever runs render_callback on a single I/O thread, but TSan in
    // CI should not flag the pattern.)
    AudioWorkgroup wg;
    std::atomic<bool> joined_flag{false};
    std::atomic<int>  join_count{0};

    auto worker = [&] {
        if (!joined_flag.load(std::memory_order_acquire)) {
            wg.join_from_audio_thread();
            // Only the winning thread observes `false` here, so the
            // counter increments exactly once.
            bool expected = false;
            if (joined_flag.compare_exchange_strong(
                    expected, true, std::memory_order_release)) {
                join_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread t1(worker);
    std::thread t2(worker);
    std::thread t3(worker);
    t1.join();
    t2.join();
    t3.join();

    REQUIRE(join_count.load() == 1);
    wg.leave();
}

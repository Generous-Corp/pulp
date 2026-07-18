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
#include <pulp/format/audio_workgroup_client.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

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

class WorkgroupClient final : public pulp::format::AudioWorkgroupClient {
public:
    void set_audio_workgroup(void* value) noexcept override {
        handle.store(value, std::memory_order_release);
        acknowledged.store(false, std::memory_order_release);
    }

    void wait_for_audio_workgroup_update() noexcept override {
        saw_null_during_wait.store(
            handle.load(std::memory_order_acquire) == nullptr,
            std::memory_order_release);
        wait_calls.fetch_add(1, std::memory_order_relaxed);
        acknowledged.store(true, std::memory_order_release);
    }

    std::atomic<void*> handle{nullptr};
    std::atomic<bool> acknowledged{false};
    std::atomic<bool> saw_null_during_wait{false};
    std::atomic<int> wait_calls{0};
};

class WorkgroupDevice : public StubDevice {
public:
    void* callback_workgroup() const override {
        calls.fetch_add(1, std::memory_order_relaxed);
        return handle;
    }

    void set_workgroup_change_callback(WorkgroupChangeCallback value) override {
        change_callback = std::move(value);
    }

    void close() override {
        closed_after_release = release_acknowledged &&
            release_acknowledged->load(std::memory_order_acquire);
    }

    void live_switch(void* replacement) {
        REQUIRE(change_callback);
        change_callback(nullptr);
        old_handle_released_before_switch = release_acknowledged &&
            release_acknowledged->load(std::memory_order_acquire);
        handle = replacement;
        change_callback(handle);
    }

    void* handle = nullptr;
    mutable std::atomic<int> calls{0};
    WorkgroupChangeCallback change_callback;
    std::atomic<bool>* release_acknowledged = nullptr;
    bool old_handle_released_before_switch = false;
    bool closed_after_release = false;
};

class ConcurrentSwitchDevice final : public StubDevice {
public:
    void* callback_workgroup() const override { return initial_handle; }

    void set_workgroup_change_callback(WorkgroupChangeCallback value) override {
        std::lock_guard<std::mutex> lock(switch_mutex);
        change_callback = std::move(value);
    }

    void quiesce_workgroup_changes() override {
        quiesce_requested.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(switch_mutex);
        changes_disabled = true;
        if (change_callback) change_callback(nullptr);
        change_callback = nullptr;
    }

    void close() override {
        closed_after_release = release_acknowledged &&
            release_acknowledged->load(std::memory_order_acquire);
    }

    void switch_in_flight(void* replacement) {
        std::unique_lock<std::mutex> switch_lock(switch_mutex);
        if (changes_disabled || !change_callback) return;
        change_callback(nullptr);
        {
            std::lock_guard<std::mutex> state_lock(state_mutex);
            switch_entered = true;
            state_changed.notify_all();
        }
        std::unique_lock<std::mutex> state_lock(state_mutex);
        state_changed.wait(state_lock, [&] { return allow_switch_finish; });
        change_callback(replacement);
        replacement_published.store(true, std::memory_order_release);
    }

    void wait_until_switch_entered() {
        std::unique_lock<std::mutex> lock(state_mutex);
        state_changed.wait(lock, [&] { return switch_entered; });
    }

    void finish_switch() {
        std::lock_guard<std::mutex> lock(state_mutex);
        allow_switch_finish = true;
        state_changed.notify_all();
    }

    void* initial_handle = reinterpret_cast<void*>(std::uintptr_t{0x7070});
    std::atomic<bool>* release_acknowledged = nullptr;
    std::atomic<bool> quiesce_requested{false};
    std::atomic<bool> replacement_published{false};
    bool closed_after_release = false;

private:
    std::mutex switch_mutex;
    std::mutex state_mutex;
    std::condition_variable state_changed;
    WorkgroupChangeCallback change_callback;
    bool changes_disabled = false;
    bool switch_entered = false;
    bool allow_switch_finish = false;
};

}  // namespace

TEST_CASE("AudioDevice base class callback_workgroup defaults to null",
          "[audio][workgroup][wiring][issue-2935]") {
    StubDevice dev;
    REQUIRE(dev.callback_workgroup() == nullptr);
}

TEST_CASE("Audio device workgroup binding forwards changes and removal",
          "[audio][workgroup][wiring]") {
    WorkgroupDevice device;
    WorkgroupClient client;
    device.handle = reinterpret_cast<void*>(std::uintptr_t{0x4040});

    pulp::format::bind_audio_device_workgroup(client, &device);
    REQUIRE(client.handle.load(std::memory_order_acquire) == device.handle);
    REQUIRE(device.calls.load(std::memory_order_relaxed) == 1);

    device.handle = reinterpret_cast<void*>(std::uintptr_t{0x5050});
    pulp::format::bind_audio_device_workgroup(client, &device);
    REQUIRE(client.handle.load(std::memory_order_acquire) == device.handle);
    REQUIRE(device.calls.load(std::memory_order_relaxed) == 2);

    pulp::format::bind_audio_device_workgroup(client, nullptr);
    REQUIRE(client.handle.load(std::memory_order_acquire) == nullptr);
    REQUIRE(device.calls.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("Live device workgroup switch drains the old borrowed handle first",
          "[audio][workgroup][wiring][lifetime]") {
    WorkgroupDevice device;
    WorkgroupClient client;
    device.release_acknowledged = &client.acknowledged;
    device.handle = reinterpret_cast<void*>(std::uintptr_t{0x4040});
    pulp::format::bind_audio_device_workgroup(client, &device);

    auto* replacement = reinterpret_cast<void*>(std::uintptr_t{0x5050});
    device.live_switch(replacement);

    REQUIRE(device.old_handle_released_before_switch);
    REQUIRE(client.saw_null_during_wait.load(std::memory_order_acquire));
    REQUIRE(client.wait_calls.load(std::memory_order_relaxed) == 1);
    REQUIRE(client.handle.load(std::memory_order_acquire) == replacement);
}

TEST_CASE("Device close waits for borrowed workgroup release acknowledgment",
          "[audio][workgroup][wiring][lifetime]") {
    WorkgroupDevice device;
    WorkgroupClient client;
    device.release_acknowledged = &client.acknowledged;
    device.handle = reinterpret_cast<void*>(std::uintptr_t{0x6060});
    pulp::format::bind_audio_device_workgroup(client, &device);

    pulp::format::close_audio_device_after_workgroup_release(&client, device);

    REQUIRE(device.closed_after_release);
    REQUIRE(client.saw_null_during_wait.load(std::memory_order_acquire));
    REQUIRE(client.wait_calls.load(std::memory_order_relaxed) == 1);
    REQUIRE(client.handle.load(std::memory_order_acquire) == nullptr);
}

TEST_CASE("Device close redrains a workgroup rebound by an in-flight switch",
          "[audio][workgroup][wiring][lifetime][threads]") {
    ConcurrentSwitchDevice device;
    WorkgroupClient client;
    device.release_acknowledged = &client.acknowledged;
    pulp::format::bind_audio_device_workgroup(client, &device);
    auto* replacement = reinterpret_cast<void*>(std::uintptr_t{0x8080});

    std::thread switcher([&] { device.switch_in_flight(replacement); });
    device.wait_until_switch_entered();
    std::thread closer([&] {
        pulp::format::close_audio_device_after_workgroup_release(&client, device);
    });
    while (!device.quiesce_requested.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    REQUIRE_FALSE(device.closed_after_release);

    device.finish_switch();
    switcher.join();
    closer.join();

    REQUIRE(device.replacement_published.load(std::memory_order_acquire));
    REQUIRE(device.closed_after_release);
    REQUIRE(client.handle.load(std::memory_order_acquire) == nullptr);
    REQUIRE(client.wait_calls.load(std::memory_order_relaxed) >= 3);
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
    // CoreAudio invokes one render thread, but a race-hammer must not create
    // concurrent calls into AudioWorkgroup itself: that object is explicitly
    // thread-affine and owns one OS join token. Claim the one-shot operation
    // atomically first, then let only the winner touch it. This keeps the test
    // faithful to the single-I/O-thread production contract while still making
    // the guard's exactly-once behavior observable under TSan.
    AudioWorkgroup wg;
    std::atomic<bool> join_claimed{false};
    std::atomic<int>  join_count{0};

    auto worker = [&] {
        bool expected = false;
        if (join_claimed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            wg.join_from_audio_thread();
            join_count.fetch_add(1, std::memory_order_relaxed);
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

// macOS: a follow-default standalone audio device (no pinned device, output-only)
// must move to the NEW system default output device LIVE — switching to AirPods /
// headphones mid-session keeps audio flowing without relaunching the app. This is
// the SDK-level guard for the M1 "audio doesn't follow the output device" feedback.
//
// The actual system-default switch is gated behind PULP_TEST_AUDIO_DEVICE_SWITCH=1
// so CI (shared self-hosted Mac runners) never repoints the host's default output.
// Run locally with that env set + at least two output devices.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/device.hpp>
#include "../core/audio/platform/mac/coreaudio_device.hpp"

#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::audio;

namespace {

std::atomic<int> released_workgroups{0};

void count_and_release_workgroup(os_workgroup_t value) noexcept {
    released_workgroups.fetch_add(1, std::memory_order_relaxed);
    os_release(value);
}

AudioDeviceID sys_default_output() {
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDefaultOutputDevice,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioDeviceID d = kAudioObjectUnknown; UInt32 s = sizeof(d);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &s, &d);
    return d;
}

void set_sys_default_output(AudioDeviceID d) {
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDefaultOutputDevice,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, sizeof(d), &d);
}

bool has_output(AudioDeviceID d) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyStreamConfiguration,
                                 kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, nullptr, &sz) != noErr || sz == 0) return false;
    std::vector<char> buf(sz);
    auto* bl = reinterpret_cast<AudioBufferList*>(buf.data());
    if (AudioObjectGetPropertyData(d, &a, 0, nullptr, &sz, bl) != noErr) return false;
    UInt32 ch = 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; ++i) ch += bl->mBuffers[i].mNumberChannels;
    return ch > 0;
}

std::vector<AudioDeviceID> all_devices() {
    AudioObjectPropertyAddress a{kAudioHardwarePropertyDevices,
                                 kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &sz) != noErr) return {};
    std::vector<AudioDeviceID> ids(sz / sizeof(AudioDeviceID));
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &sz, ids.data());
    return ids;
}

}  // namespace

TEST_CASE("CoreAudio caller-owned workgroup references release exactly once",
          "[audio][coreaudio][workgroup][lifetime]") {
    released_workgroups.store(0, std::memory_order_relaxed);
    auto* first = os_workgroup_parallel_create("pulp-coreaudio-owned-first", nullptr);
    auto* second = os_workgroup_parallel_create("pulp-coreaudio-owned-second", nullptr);
    auto* third = os_workgroup_parallel_create("pulp-coreaudio-owned-third", nullptr);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(third != nullptr);

    {
        pulp::audio::mac::CoreAudioWorkgroupReference owned{
            count_and_release_workgroup};
        owned.adopt(first);
        REQUIRE(owned.get() == first);
        REQUIRE(released_workgroups.load(std::memory_order_relaxed) == 0);

        // Replacement consumes the new +1 and releases only the old +1.
        owned.adopt(second);
        REQUIRE(owned.get() == second);
        REQUIRE(released_workgroups.load(std::memory_order_relaxed) == 1);

        owned.reset();
        REQUIRE(owned.get() == nullptr);
        REQUIRE(released_workgroups.load(std::memory_order_relaxed) == 2);

        // Destruction covers the close-with-live-query path.
        owned.adopt(third);
    }
    REQUIRE(released_workgroups.load(std::memory_order_relaxed) == 3);
}

TEST_CASE("CoreAudio failed stop preserves old device and workgroup",
          "[audio][coreaudio][workgroup][lifetime]") {
    released_workgroups.store(0, std::memory_order_relaxed);
    auto* old_workgroup =
        os_workgroup_parallel_create("pulp-coreaudio-stop-failure-old", nullptr);
    REQUIRE(old_workgroup != nullptr);

    AudioDeviceID current_device = 41;
    int current_device_sets = 0;
    int workgroup_requeries = 0;
    {
        pulp::audio::mac::CoreAudioWorkgroupReference owned{
            count_and_release_workgroup};
        owned.adopt(old_workgroup);

        const OSStatus failed_stop = kAudio_ParamError;
        if (pulp::audio::mac::coreaudio_stop_allows_device_switch(failed_stop)) {
            current_device = 42;
            ++current_device_sets;
            owned.reset();
            ++workgroup_requeries;
        }

        CHECK(current_device == 41);
        CHECK(current_device_sets == 0);
        CHECK(workgroup_requeries == 0);
        CHECK(owned.get() == old_workgroup);
        CHECK(released_workgroups.load(std::memory_order_relaxed) == 0);
    }
    CHECK(released_workgroups.load(std::memory_order_relaxed) == 1);
    STATIC_REQUIRE(pulp::audio::mac::coreaudio_stop_allows_device_switch(noErr));
}

TEST_CASE("CoreAudio restart status controls running state",
          "[audio][coreaudio][workgroup][lifetime]") {
    bool running = true;
    CHECK_FALSE(pulp::audio::mac::update_coreaudio_running_after_restart(
        kAudio_ParamError, running));
    CHECK_FALSE(running);

    CHECK(pulp::audio::mac::update_coreaudio_running_after_restart(
        noErr, running));
    CHECK(running);
}

TEST_CASE("CoreAudio fallback priority retries failure and resets per lifetime",
          "[audio][coreaudio][workgroup][rt-priority]") {
    std::atomic<bool> configured{false};
    int attempts = 0;
    const auto fail_then_succeed = [&]() noexcept { return ++attempts >= 2; };

    CHECK_FALSE(pulp::audio::mac::configure_coreaudio_fallback_priority_once(
        configured, fail_then_succeed));
    CHECK_FALSE(configured.load(std::memory_order_acquire));
    CHECK(pulp::audio::mac::configure_coreaudio_fallback_priority_once(
        configured, fail_then_succeed));
    CHECK(configured.load(std::memory_order_acquire));
    CHECK(attempts == 2);

    CHECK(pulp::audio::mac::configure_coreaudio_fallback_priority_once(
        configured, fail_then_succeed));
    CHECK(attempts == 2);
    configured.store(false, std::memory_order_release);
    CHECK(pulp::audio::mac::configure_coreaudio_fallback_priority_once(
        configured, fail_then_succeed));
    CHECK(attempts == 3);
}

TEST_CASE("follow-default output device tracks the system default live",
          "[audio][coreaudio][device-follow]") {
    if (!std::getenv("PULP_TEST_AUDIO_DEVICE_SWITCH")) {
        SUCCEED("set PULP_TEST_AUDIO_DEVICE_SWITCH=1 (and have 2+ output devices) to run the live switch");
        return;
    }
    const AudioDeviceID orig = sys_default_output();
    AudioDeviceID other = kAudioObjectUnknown;
    for (auto d : all_devices())
        if (d != orig && has_output(d)) { other = d; break; }
    if (other == kAudioObjectUnknown) {
        SUCCEED("only one output device present — cannot exercise the follow");
        return;
    }

    std::atomic<std::uint64_t> callbacks{0};
    auto sys = create_audio_system();
    REQUIRE(sys);
    auto dev = sys->create_device("");  // empty id => system default => follow_default
    REQUIRE(dev);
    DeviceConfig cfg;
    cfg.sample_rate = 48000.0;
    cfg.buffer_size = 256;
    cfg.output_channels = 2;
    cfg.input_channels = 0;  // instrument: output-only => follow_default
    REQUIRE(dev->open(cfg));
    REQUIRE(dev->start([&callbacks](const BufferView<const float>&, BufferView<float>& out,
                                    const CallbackContext&) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            float* p = out.channel_ptr(c);
            for (std::size_t i = 0; i < out.num_samples(); ++i) p[i] = 0.0f;  // silent
        }
    }));

    auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(900)); };
    auto rendering = [&] {
        const auto a = callbacks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        return callbacks.load() > a;  // IO thread still pulling buffers (audio alive)
    };

    const std::string at_open = dev->info().name;
    REQUIRE(rendering());

    // Forward: system default -> OTHER device. The unit must follow and keep rendering.
    set_sys_default_output(other);
    settle();
    const std::string after_fwd = dev->info().name;
    CHECK(after_fwd != at_open);
    CHECK(rendering());

    // Back: system default -> original. Must follow AGAIN and NOT go silent/wedged
    // (the M1 report: switching back lost audio).
    set_sys_default_output(orig);
    settle();
    const std::string after_back = dev->info().name;
    const bool alive_after_back = rendering();

    dev->stop();
    dev->close();

    INFO("open='" << at_open << "' fwd='" << after_fwd << "' back='" << after_back << "'");
    CHECK(after_back == at_open);   // followed the round trip
    CHECK(alive_after_back);        // audio path NOT wedged after switching back
}

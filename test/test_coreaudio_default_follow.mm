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

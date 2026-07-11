// macOS: opening a CoreAudio device input-only — input channels requested, zero
// output channels — must SUCCEED. AUHAL rejects a zero-channel output stream
// format (kAudioUnitErr_FormatNotSupported, -10868), so the backend disables
// output IO on bus 0 and drives the unit from an input callback instead of a
// render callback. A capture / metering / analysis tool depends on this: "the
// outputs are not open" is a strictly stronger safety guarantee than "the
// outputs are open but we promise to write zeros".
//
// This test is capture-only and read-only: it never emits a sample and never
// repoints a system default, so it is safe on shared CI. It skips cleanly (with
// a reason) when the host has no input-capable device — e.g. a headless Mac with
// no microphone. When an input device IS present it runs for real, so it is not
// a no-op on CI.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/device.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::audio;

namespace {

// First input-capable device, preferring the system default input.
bool find_input_device(AudioSystem& sys, DeviceInfo& out) {
    const auto devs = sys.enumerate_devices();
    for (const auto& d : devs)
        if (d.max_input_channels > 0 && d.is_default_input) { out = d; return true; }
    for (const auto& d : devs)
        if (d.max_input_channels > 0) { out = d; return true; }
    return false;
}

// Every stereo-capable output device that is NOT the system default.
//
// A test has no business grabbing whatever the developer is listening through,
// and on a machine whose default output is a DC-coupled interface wired to a
// modular synthesizer, "whatever the default is" reaches a patch cable.
//
// All of them, not the first: the first non-default output on a real desk is as
// likely as not an HDMI display, which opens cleanly and then will not start.
// Stereo-capable, because the caller asks for two channels and a mono device
// would fail `open` for a reason that has nothing to do with what is under test.
std::vector<DeviceInfo> non_default_output_devices(AudioSystem& sys) {
    std::vector<DeviceInfo> out;
    for (const auto& d : sys.enumerate_devices())
        if (d.max_output_channels >= 2 && !d.is_default_output) out.push_back(d);
    return out;
}

/// Adopt whatever rate the device is already running at, rather than asking for
/// one.
///
/// `open()` only calls `set_nominal_sample_rate` when a positive rate is
/// requested that differs from the current one, so zero means "do not touch the
/// clock" and the actual rate is written back into the config. This matters:
/// `DeviceInfo::sample_rates` is an unordered capability list, and its first
/// entry is routinely NOT the rate the device is running. Reconfiguring a
/// converter's clock from a unit test drops any ADAT link hanging off it — a
/// failure that presents as a bad cable.
constexpr double kAdoptCurrentRate = 0.0;

struct RunObservation {
    bool started = false;             // the device actually began running
    std::uint64_t callbacks = 0;      // total callbacks fired
    std::uint64_t input_frames = 0;   // frames delivered on the input view
    int input_channels = -1;          // channels seen on the input view (-1 = never fired)
    int output_channels = -1;         // channels seen on the output view (-1 = never fired)
};

// Start the device, hold every output channel at zero, observe the callback for
// up to ~2s (or until it has clearly fired), then stop. Emits nothing. All
// observation goes through atomics; no Catch2 assertion runs on the audio
// thread (that would be undefined behavior).
RunObservation run_briefly(AudioDevice& dev) {
    std::atomic<std::uint64_t> cbs{0};
    std::atomic<std::uint64_t> in_frames{0};
    std::atomic<int> in_ch{-1};
    std::atomic<int> out_ch{-1};

    const bool started = dev.start(
        [&](const BufferView<const float>& in, BufferView<float>& out, const CallbackContext&) {
            cbs.fetch_add(1, std::memory_order_relaxed);
            out_ch.store(static_cast<int>(out.num_channels()), std::memory_order_relaxed);
            in_ch.store(static_cast<int>(in.num_channels()), std::memory_order_relaxed);
            // Hold outputs at zero. For an input-only unit this loop runs zero
            // times because the output view is empty.
            for (std::size_t c = 0; c < out.num_channels(); ++c) {
                float* p = out.channel_ptr(c);
                for (std::size_t i = 0; i < out.num_samples(); ++i) p[i] = 0.0f;
            }
            if (in.num_channels() > 0)
                in_frames.fetch_add(in.num_samples(), std::memory_order_relaxed);
        });
    // Reported, not asserted. Some output-capable devices -- an HDMI display
    // sink with no active audio stream, for one -- open cleanly and then refuse
    // to start. Whether that is a bug or a fact about the host is the caller's
    // question, not this helper's.
    if (!started) return {};

    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (cbs.load(std::memory_order_relaxed) < 4 &&
           std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    dev.stop();

    return {started, cbs.load(), in_frames.load(),
            in_ch.load(std::memory_order_relaxed),
            out_ch.load(std::memory_order_relaxed)};
}

}  // namespace

TEST_CASE("CoreAudio opens a device input-only and delivers captured frames",
          "[audio][coreaudio][input-only]") {
    auto sys = create_audio_system();
    REQUIRE(sys);

    DeviceInfo in_dev;
    if (!find_input_device(*sys, in_dev)) {
        SUCCEED("no input-capable audio device present — cannot exercise input-only open");
        return;
    }

    SECTION("input-only open succeeds and captures") {
        auto dev = sys->create_device(in_dev.id);
        REQUIRE(dev);
        DeviceConfig cfg;
        cfg.device_id = in_dev.id;
        cfg.sample_rate = kAdoptCurrentRate;
        cfg.buffer_size = 256;
        cfg.input_channels = 1;   // capture at least one channel
        cfg.output_channels = 0;  // the configuration that regressed with -10868

        // The fix: this open must succeed. Before the fix it returned false with
        // "could not set output stream format (-10868)".
        REQUIRE(dev->open(cfg));
        REQUIRE(dev->is_open());

        const RunObservation obs = run_briefly(*dev);
        dev->close();

        INFO("callbacks=" << obs.callbacks
             << " input_frames=" << obs.input_frames
             << " input_channels=" << obs.input_channels
             << " output_channels=" << obs.output_channels);

        // The input callback fired and delivered input frames (the samples may be
        // a noise floor or, without microphone permission, zeros — either way the
        // frames are delivered).
        REQUIRE(obs.started);
        CHECK(obs.callbacks > 0);
        CHECK(obs.input_channels >= 1);
        CHECK(obs.input_frames > 0);
        // Output is not open: the caller receives an empty output view.
        CHECK(obs.output_channels == 0);
    }

    SECTION("output-only open still succeeds (regression guard)") {
        const auto candidates = non_default_output_devices(*sys);
        if (candidates.empty()) {
            SUCCEED("no non-default stereo output device present");
            return;
        }

        // Opening is what this section guards, and it must succeed on every
        // candidate — the input-only fix must not have cost the output path.
        // *Starting* is a fact about the device: an HDMI sink with no active
        // audio stream opens and then returns -536870198 from AudioDeviceStart.
        // So walk the candidates until one runs, and only give up if none do.
        bool observed = false;
        for (const auto& out_dev : candidates) {
            auto dev = sys->create_device(out_dev.id);
            REQUIRE(dev);
            DeviceConfig cfg;
            cfg.device_id = out_dev.id;
            cfg.sample_rate = kAdoptCurrentRate;
            cfg.buffer_size = 256;
            cfg.input_channels = 0;
            cfg.output_channels = 2;

            INFO("device=" << out_dev.name);
            REQUIRE(dev->open(cfg));
            REQUIRE(dev->is_open());

            const RunObservation obs = run_briefly(*dev);
            dev->close();
            if (!obs.started) continue;

            INFO("callbacks=" << obs.callbacks
                 << " output_channels=" << obs.output_channels);
            CHECK(obs.callbacks > 0);
            CHECK(obs.output_channels == 2);
            observed = true;
            break;
        }

        // Every candidate opened — the guard held — but none would run. Say so
        // rather than pass quietly: a skip is not a pass.
        if (!observed)
            WARN("every non-default stereo output device opened, but none would "
                 "start; the callback assertions did not run on this host");
    }

    SECTION("duplex open still succeeds (regression guard)") {
        // A single device that has BOTH input and output can be opened duplex.
        DeviceInfo duplex;
        bool have_duplex = false;
        for (const auto& d : sys->enumerate_devices()) {
            if (d.max_input_channels > 0 && d.max_output_channels > 0) {
                duplex = d;
                have_duplex = true;
                break;
            }
        }
        if (!have_duplex) {
            SUCCEED("no duplex-capable device present — cannot exercise duplex open");
            return;
        }

        auto dev = sys->create_device(duplex.id);
        REQUIRE(dev);
        DeviceConfig cfg;
        cfg.device_id = duplex.id;
        cfg.sample_rate = kAdoptCurrentRate;
        cfg.buffer_size = 256;
        cfg.input_channels = 1;
        cfg.output_channels = 2;

        REQUIRE(dev->open(cfg));
        REQUIRE(dev->is_open());

        const RunObservation obs = run_briefly(*dev);
        dev->close();

        INFO("callbacks=" << obs.callbacks
             << " input_frames=" << obs.input_frames
             << " input_channels=" << obs.input_channels
             << " output_channels=" << obs.output_channels);
        REQUIRE(obs.started);
        CHECK(obs.callbacks > 0);
        CHECK(obs.output_channels == 2);
        CHECK(obs.input_channels >= 1);
        CHECK(obs.input_frames > 0);
    }
}

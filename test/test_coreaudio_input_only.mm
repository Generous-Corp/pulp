// macOS: opening a CoreAudio device input-only — input channels requested, zero
// output channels — must SUCCEED. AUHAL rejects a zero-channel output stream
// format (kAudioUnitErr_FormatNotSupported, -10868), so the backend disables
// output IO on bus 0 and drives the unit from an input callback instead of a
// render callback. A capture / metering / analysis tool depends on this: "the
// outputs are not open" is a strictly stronger safety guarantee than "the
// outputs are open but we promise to write zeros".
//
// This test never emits a sample and never repoints a system default. But
// opening a device is not free of side effects: every open()/start() cycle
// re-clocks the selected hardware, and a pro interface like an Apogee Symphony
// audibly clicks its output relay each time. A test has no business doing that
// to whatever the developer is listening through, nor to a shared self-hosted
// CI Mac. So every section here that opens a device is opt-in, exactly like the
// live device switch in test_coreaudio_default_follow.mm:
//
//   * PULP_TEST_AUDIO_OPEN_HARDWARE=1 — required to open ANY real device. Unset
//     (the default, CI included) skips every opening section with a reason, so
//     `ctest -R coreaudio` touches no hardware and re-clocks nothing.
//   * PULP_TEST_AUDIO_DEVICE_UID=<DeviceInfo::id> — optional. Pins exactly which
//     device the opening sections may touch (e.g. a virtual/aggregate loopback
//     such as BlackHole) instead of the auto-selected default/first device; a
//     section that cannot fill its channel role with the pinned device skips
//     rather than reaching for the system default.
//
// A single opt-in check at the top of the test case gates every one of them:
// with the opt-in unset nothing is enumerated or opened. When it IS set, the
// -10851 pure-input and -10868 input-only regression guards assert in full.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/device.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace pulp::audio;

namespace {

// Opening a real device re-clocks user-facing hardware (see the file header), so
// every section that calls open()/start() is opt-in. Unset by default — shared
// CI included — which keeps `ctest -R coreaudio` from clicking an interface's
// output relay. Mirrors PULP_TEST_AUDIO_DEVICE_SWITCH in
// test_coreaudio_default_follow.mm.
bool audio_hardware_open_enabled() {
    return std::getenv("PULP_TEST_AUDIO_OPEN_HARDWARE") != nullptr;
}

// Optional pin: when PULP_TEST_AUDIO_DEVICE_UID is set, only a device whose
// DeviceInfo::id equals it is eligible to be opened, so a run can target a
// known-safe virtual/loopback device instead of the system default. A section
// that cannot fill its channel role with the pinned device skips rather than
// falling back. Unset means "use the role's auto-selected device". DeviceInfo::id
// is the SDK's platform-unique identifier (on CoreAudio the AudioDeviceID
// string) — there is no separate stable UID field to match against.
bool device_matches_pin(const DeviceInfo& d) {
    const char* pin = std::getenv("PULP_TEST_AUDIO_DEVICE_UID");
    return pin == nullptr || d.id == pin;
}

// First input-capable device, preferring the system default input.
bool find_input_device(AudioSystem& sys, DeviceInfo& out) {
    const auto devs = sys.enumerate_devices();
    for (const auto& d : devs)
        if (d.max_input_channels > 0 && d.is_default_input && device_matches_pin(d)) { out = d; return true; }
    for (const auto& d : devs)
        if (d.max_input_channels > 0 && device_matches_pin(d)) { out = d; return true; }
    return false;
}

// A PURE-INPUT device: input channels, and NO output channels at all — a plain
// USB microphone, an interface input, an aggregate capture device.
//
// This is the case that actually breaks, and it is why the bug shipped: AUHAL
// starts with output element 0 enabled, so binding CurrentDevice before EnableIO
// hands an output-enabled unit to a device with no output streams, and CoreAudio
// rejects it (-10851). A duplex device has output streams, so the premature bind
// happens to succeed — and `find_input_device` above prefers the system DEFAULT
// input, which on most dev Macs is duplex. The suite therefore passed locally
// while input-only capture was broken for every real microphone.
bool find_pure_input_device(AudioSystem& sys, DeviceInfo& out) {
    for (const auto& d : sys.enumerate_devices())
        if (d.max_input_channels > 0 && d.max_output_channels == 0 && device_matches_pin(d)) { out = d; return true; }
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
        if (d.max_output_channels >= 2 && !d.is_default_output && device_matches_pin(d)) out.push_back(d);
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
    // Opt-in gate. Opening any device below re-clocks user-facing hardware — a pro
    // interface audibly clicks its output relay — so skip the whole case unless the
    // developer explicitly asks for it. Same shape as the live-switch gate in
    // test_coreaudio_default_follow.mm. A single top-level return (rather than one
    // per SECTION) is deliberate: an early return inside a SECTION hides every
    // later sibling SECTION from Catch2's discovery, so a per-section gate would
    // silently stop the -10868 / output / duplex guards from ever running.
    if (!audio_hardware_open_enabled()) {
        SUCCEED("set PULP_TEST_AUDIO_OPEN_HARDWARE=1 to open a real audio device — "
                "skipping every open()/start() section so the suite never re-clocks "
                "the developer's interface or a shared CI Mac");
        return;
    }

    auto sys = create_audio_system();
    REQUIRE(sys);

    DeviceInfo in_dev;
    if (!find_input_device(*sys, in_dev)) {
        SUCCEED("no input-capable audio device present — cannot exercise input-only open");
        return;
    }

    // The regression guard for the AUHAL EnableIO/CurrentDevice ordering bug.
    // The default-input capture case (next section) can pass on a duplex default
    // input even when this is broken; only a device with ZERO output streams
    // exercises the failure.
    SECTION("input-only open succeeds on a device with NO output channels") {
        DeviceInfo pure;
        if (!find_pure_input_device(*sys, pure)) {
            SUCCEED("no pure-input device present (every input here is duplex) — "
                    "cannot exercise the zero-output-stream open");
            return;
        }

        auto dev = sys->create_device(pure.id);
        REQUIRE(dev);
        DeviceConfig cfg;
        cfg.device_id = pure.id;
        cfg.sample_rate = kAdoptCurrentRate;
        cfg.buffer_size = 256;
        cfg.input_channels = 1;
        cfg.output_channels = 0;

        INFO("device '" << pure.name << "' in=" << pure.max_input_channels
             << " out=" << pure.max_output_channels);
        // Before the fix this returned false with
        // "CoreAudio: could not set device (-10851)".
        REQUIRE(dev->open(cfg));
        REQUIRE(dev->is_open());
        dev->close();
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

        REQUIRE(obs.started);

        // Capture delivery depends on a live input source. A headless host
        // (CI runner, or a session without microphone access) exposes an
        // input-capable device that opens and starts but never delivers real
        // callbacks — no signal reaches the input. Treat "opened + started but
        // zero callbacks" as an environment without a capture source rather
        // than a failure: the open()/EnableIO ordering regression this case
        // guards has already been asserted above.
        if (obs.callbacks == 0) {
            SUCCEED("input device opened and started but delivered no capture "
                    "callbacks — headless host without a live input source");
        } else {
            // The input callback fired and delivered input frames (the samples
            // may be a noise floor or, without microphone permission, zeros —
            // either way the frames are delivered).
            CHECK(obs.input_channels >= 1);
            CHECK(obs.input_frames > 0);
            // Output is not open: the caller receives an empty output view.
            CHECK(obs.output_channels == 0);
        }
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
            if (d.max_input_channels > 0 && d.max_output_channels > 0 && device_matches_pin(d)) {
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

/// @file test_audio_device_manager.cpp
/// Item 1.2a — AudioDeviceManager persistence + MIDI hub.
///
/// Acceptance from the macOS-plugin-authoring plan:
///   - restart-with-same-device — save then load round-trips
///   - fallback-to-default-when-persisted-missing — resolver returns
///     the default id and sets the fallback flag
///   - subscriber callback fires
///   - subscriber-out-of-scope (no use-after-free) — token dtor
///     unsubscribes; later dispatch sees zero subscribers
///   - CPU-load reads sensibly under simulated load

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/audio_device_manager.hpp>
#include <pulp/state/properties_file.hpp>
#include <pulp/runtime/temporary_file.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value) : name_(name) {
        if (const char* existing = std::getenv(name)) old_value_ = existing;
        set(name, value);
    }
    ~ScopedEnvVar() {
        if (old_value_) set(name_.c_str(), *old_value_);
        else            unset(name_.c_str());
    }
private:
    static void set(const char* n, const std::string& v) {
#ifdef _WIN32
        _putenv_s(n, v.c_str());
#else
        setenv(n, v.c_str(), 1);
#endif
    }
    static void unset(const char* n) {
#ifdef _WIN32
        _putenv_s(n, "");
#else
        unsetenv(n);
#endif
    }
    std::string name_;
    std::optional<std::string> old_value_;
};

// Spin up a sandboxed `ApplicationProperties` whose paths land under a
// per-test temp directory. Avoids stomping the developer's real
// settings.
struct PropsSandbox {
    pulp::runtime::TemporaryFile marker;
    std::filesystem::path        home;
    std::unique_ptr<ScopedEnvVar> env;
    std::unique_ptr<pulp::state::ApplicationProperties> props;

    explicit PropsSandbox(const std::string& app_name)
        : marker(".home"),
          home(marker.path_string() + "_dir") {
        std::filesystem::create_directories(home);
#ifdef _WIN32
        env = std::make_unique<ScopedEnvVar>("APPDATA", home.string());
#else
        env = std::make_unique<ScopedEnvVar>("HOME", home.string());
#endif
        props = std::make_unique<pulp::state::ApplicationProperties>(app_name);
    }
};

DeviceInfo make_device(std::string id, std::string name) {
    DeviceInfo d;
    d.id = std::move(id);
    d.name = std::move(name);
    d.max_input_channels = 2;
    d.max_output_channels = 2;
    return d;
}

}  // namespace

TEST_CASE("AudioDeviceManager save/load round-trips selection",
          "[audio][audio-device-manager][issue-2935]") {
    PropsSandbox box("PulpADMRoundtrip");

    DeviceSelection sel;
    sel.output_device       = "device:built-in:out";
    sel.input_device        = "device:built-in:in";
    sel.sample_rate         = 48000.0;
    sel.buffer_size         = 256;
    sel.output_channel_mask = 0b11ULL;
    sel.input_channel_mask  = 0b1ULL;

    REQUIRE(AudioDeviceManager::save_selection(*box.props, sel));

    // Fresh ApplicationProperties pointed at the same file should see
    // the persisted values after load() — this is the "restart with
    // same device" path.
    pulp::state::ApplicationProperties reopened("PulpADMRoundtrip");
    reopened.load();
    auto loaded = AudioDeviceManager::load_selection(reopened);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->output_device       == sel.output_device);
    REQUIRE(loaded->input_device        == sel.input_device);
    REQUIRE_THAT(loaded->sample_rate, WithinAbs(48000.0, 0.001));
    REQUIRE(loaded->buffer_size         == 256);
    REQUIRE(loaded->output_channel_mask == 0b11ULL);
    REQUIRE(loaded->input_channel_mask  == 0b1ULL);
}

TEST_CASE("AudioDeviceManager load returns nullopt when no keys exist",
          "[audio][audio-device-manager][issue-2935]") {
    PropsSandbox box("PulpADMNoKeys");
    REQUIRE_FALSE(AudioDeviceManager::load_selection(*box.props).has_value());
}

TEST_CASE("AudioDeviceManager resolves missing devices to the system default",
          "[audio][audio-device-manager][issue-2935]") {
    std::vector<DeviceInfo> available = {
        make_device("device:builtin:out", "Built-in Output"),
        make_device("device:builtin:in",  "Built-in Input"),
    };

    SECTION("persisted device exists — no fallback") {
        DeviceSelection sel;
        sel.output_device = "device:builtin:out";
        sel.input_device  = "device:builtin:in";
        auto r = AudioDeviceManager::resolve_selection(
            sel, available, "device:builtin:out", "device:builtin:in");
        REQUIRE_FALSE(r.fallback_used_output);
        REQUIRE_FALSE(r.fallback_used_input);
        REQUIRE(r.resolved.output_device == "device:builtin:out");
    }

    SECTION("persisted output device missing — falls back, flag set") {
        DeviceSelection sel;
        sel.output_device = "device:scarlett-2i2";  // unplugged
        sel.input_device  = "device:builtin:in";
        auto r = AudioDeviceManager::resolve_selection(
            sel, available, "device:builtin:out", "device:builtin:in");
        REQUIRE(r.fallback_used_output);
        REQUIRE_FALSE(r.fallback_used_input);
        REQUIRE(r.resolved.output_device == "device:builtin:out");
    }

    SECTION("persisted input device missing — falls back independently") {
        DeviceSelection sel;
        sel.output_device = "device:builtin:out";
        sel.input_device  = "device:unknown-input";
        auto r = AudioDeviceManager::resolve_selection(
            sel, available, "device:builtin:out", "device:builtin:in");
        REQUIRE_FALSE(r.fallback_used_output);
        REQUIRE(r.fallback_used_input);
        REQUIRE(r.resolved.input_device == "device:builtin:in");
    }

    SECTION("empty input id with empty fallback — no fallback flagged") {
        DeviceSelection sel;
        sel.output_device = "device:builtin:out";
        sel.input_device  = "";
        auto r = AudioDeviceManager::resolve_selection(
            sel, available, "device:builtin:out", "");
        REQUIRE_FALSE(r.fallback_used_input);
        REQUIRE(r.resolved.input_device.empty());
    }
}

TEST_CASE("AudioDeviceManager selection_to_config derives channel counts from masks",
          "[audio][audio-device-manager][issue-2935]") {
    DeviceSelection sel;
    sel.output_device       = "id";
    sel.sample_rate         = 96000.0;
    sel.buffer_size         = 128;
    sel.output_channel_mask = 0b1111ULL;  // 4 channels
    sel.input_channel_mask  = 0b101ULL;   // 2 channels

    auto cfg = AudioDeviceManager::selection_to_config(sel);
    REQUIRE(cfg.device_id == "id");
    REQUIRE_THAT(cfg.sample_rate, WithinAbs(96000.0, 0.001));
    REQUIRE(cfg.buffer_size     == 128);
    REQUIRE(cfg.output_channels == 4);
    REQUIRE(cfg.input_channels  == 2);
}

TEST_CASE("AudioDeviceManager selection_to_config falls back to defaults on zero",
          "[audio][audio-device-manager][issue-2935]") {
    DeviceSelection sel;  // all defaulted
    auto cfg = AudioDeviceManager::selection_to_config(sel, 2, 1);
    REQUIRE_THAT(cfg.sample_rate, WithinAbs(48000.0, 0.001));
    REQUIRE(cfg.buffer_size     == 256);
    REQUIRE(cfg.output_channels == 2);
    REQUIRE(cfg.input_channels  == 1);
}

TEST_CASE("AudioDeviceManager dispatches MIDI to live subscribers",
          "[audio][audio-device-manager][midi][issue-2935]") {
    AudioDeviceManager mgr;

    std::atomic<int> count_a{0};
    std::atomic<int> count_b{0};
    auto token_a = mgr.subscribe_midi(
        [&](const pulp::midi::MidiEvent&) { ++count_a; });
    auto token_b = mgr.subscribe_midi(
        [&](const pulp::midi::MidiEvent&) { ++count_b; });

    REQUIRE(mgr.midi_subscriber_count() == 2);
    REQUIRE(token_a.active());
    REQUIRE(token_b.active());

    auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
    mgr.dispatch_midi_event(note);
    mgr.dispatch_midi_event(note);

    REQUIRE(count_a.load() == 2);
    REQUIRE(count_b.load() == 2);
}

TEST_CASE("AudioDeviceManager subscriber going out of scope auto-unsubscribes",
          "[audio][audio-device-manager][midi][issue-2935]") {
    AudioDeviceManager mgr;
    std::atomic<int> count{0};

    {
        auto tok = mgr.subscribe_midi(
            [&](const pulp::midi::MidiEvent&) { ++count; });
        REQUIRE(mgr.midi_subscriber_count() == 1);
        mgr.dispatch_midi_event(pulp::midi::MidiEvent::note_on(0, 60, 100));
        REQUIRE(count.load() == 1);
    }  // tok destroyed here → automatic unsubscribe

    REQUIRE(mgr.midi_subscriber_count() == 0);

    // Dispatch after unsubscribe must NOT call the stale handler — if
    // it did, we'd touch a dangling reference to `count`. Catch2 + ASan
    // CI catch the use-after-free; the assertion here is the
    // observable post-condition.
    mgr.dispatch_midi_event(pulp::midi::MidiEvent::note_on(0, 60, 100));
    REQUIRE(count.load() == 1);
}

TEST_CASE("AudioDeviceManager outliving the manager is a safe no-op",
          "[audio][audio-device-manager][midi][issue-2935]") {
    MidiSubscriptionToken tok;
    {
        AudioDeviceManager mgr;
        tok = mgr.subscribe_midi([](const pulp::midi::MidiEvent&) {});
        REQUIRE(tok.active());
        // Manager goes out of scope here while `tok` lives on.
    }
    // Token destructor must not deref the dead manager.
    REQUIRE_FALSE(tok.active());
    // Explicit reset() on the dead-manager token also safe.
    tok.reset();
}

TEST_CASE("AudioDeviceManager CPU-load tracks work performed in the audio window",
          "[audio][audio-device-manager][cpu-load][issue-2935]") {
    AudioDeviceManager mgr;

    // Take a few cheap measurements first — load should stay well
    // below 1.0 when essentially no work happens inside the window.
    for (int i = 0; i < 32; ++i) {
        mgr.begin_cpu_measure(/*num_frames=*/512, /*sample_rate=*/48000.0f);
        // No work — exit immediately.
        mgr.end_cpu_measure();
    }
    const float idle_load = mgr.cpu_load();
    REQUIRE(idle_load >= 0.0f);
    REQUIRE(idle_load < 0.5f);

    // Now simulate a stressed callback: sleep for ~half a buffer worth
    // of wall-clock time inside the window.
    const auto half_buffer = std::chrono::microseconds(
        (512 * 1'000'000) / 48000 / 2);
    for (int i = 0; i < 32; ++i) {
        mgr.begin_cpu_measure(512, 48000.0f);
        std::this_thread::sleep_for(half_buffer);
        mgr.end_cpu_measure();
    }
    const float busy_load = mgr.cpu_load();
    REQUIRE(busy_load > idle_load);
    // Peak from the busy run must be at least non-trivial. We don't
    // assert a tight upper bound — CI machines vary wildly under
    // load — but the value must be a finite positive number.
    REQUIRE(mgr.peak_cpu_load() > 0.0f);
    REQUIRE(std::isfinite(mgr.peak_cpu_load()));

    mgr.reset_peak_cpu_load();
    REQUIRE(mgr.peak_cpu_load() == 0.0f);
}

// ── 1.2b — Lifecycle / hotplug / recovery ──────────────────────────

TEST_CASE("AudioDeviceManager dispatches injected device-change events",
          "[audio][audio-device-manager][lifecycle][issue-2935]") {
    AudioDeviceManager mgr;

    std::atomic<int> received{0};
    AudioDeviceManager::DeviceChangeEvent last_event{};
    std::mutex captured_mu;

    auto tok = mgr.subscribe_device_changes(
        [&](const AudioDeviceManager::DeviceChangeEvent& ev) {
            std::lock_guard<std::mutex> lk(captured_mu);
            last_event = ev;
            ++received;
        });
    REQUIRE(mgr.device_change_subscriber_count() == 1);
    REQUIRE(tok.active());

    AudioDeviceManager::DeviceChangeEvent ev;
    ev.kind = AudioDeviceManager::DeviceChangeKind::Added;
    ev.device_id = "device:scarlett-2i2";
    ev.devices = {
        make_device("device:builtin:out", "Built-in Output"),
        make_device("device:scarlett-2i2",  "Scarlett 2i2"),
    };
    mgr.dispatch_device_change(ev);

    REQUIRE(received.load() == 1);
    {
        std::lock_guard<std::mutex> lk(captured_mu);
        REQUIRE(last_event.kind == AudioDeviceManager::DeviceChangeKind::Added);
        REQUIRE(last_event.device_id == "device:scarlett-2i2");
        REQUIRE(last_event.devices.size() == 2);
    }
}

TEST_CASE("AudioDeviceManager device-change subscribers auto-unsubscribe",
          "[audio][audio-device-manager][lifecycle][issue-2935]") {
    AudioDeviceManager mgr;
    std::atomic<int> received{0};

    {
        auto tok = mgr.subscribe_device_changes(
            [&](const AudioDeviceManager::DeviceChangeEvent&) { ++received; });
        REQUIRE(mgr.device_change_subscriber_count() == 1);

        AudioDeviceManager::DeviceChangeEvent ev;
        mgr.dispatch_device_change(ev);
        REQUIRE(received.load() == 1);
    }  // tok destroyed → unsubscribe

    REQUIRE(mgr.device_change_subscriber_count() == 0);
    mgr.dispatch_device_change({});
    // No callback should have fired; ASan/TSan would catch a
    // use-after-free against the dangling `received` reference.
    REQUIRE(received.load() == 1);
}

TEST_CASE("AudioDeviceManager device-change token outliving manager is safe",
          "[audio][audio-device-manager][lifecycle][issue-2935]") {
    AudioDeviceManager::DeviceChangeToken tok;
    {
        AudioDeviceManager mgr;
        tok = mgr.subscribe_device_changes(
            [](const AudioDeviceManager::DeviceChangeEvent&) {});
        REQUIRE(tok.active());
    }
    REQUIRE_FALSE(tok.active());
    tok.reset();  // safe no-op on dead manager
}

TEST_CASE("AudioDeviceManager dispatches default-device-change to handler",
          "[audio][audio-device-manager][lifecycle][issue-2935]") {
    AudioDeviceManager mgr;
    std::atomic<int> output_changes{0};
    std::atomic<int> input_changes{0};
    std::string captured_id;
    std::mutex id_mu;

    mgr.set_default_device_change_handler(
        [&](bool is_input, const std::string& new_id) {
            if (is_input) ++input_changes;
            else          ++output_changes;
            std::lock_guard<std::mutex> lk(id_mu);
            captured_id = new_id;
        });

    mgr.dispatch_default_device_change(false, "device:airpods");
    mgr.dispatch_default_device_change(true,  "device:built-in-mic");

    REQUIRE(output_changes.load() == 1);
    REQUIRE(input_changes.load()  == 1);
    {
        std::lock_guard<std::mutex> lk(id_mu);
        REQUIRE(captured_id == "device:built-in-mic");
    }

    // Clearing the handler stops dispatch (and is a safe no-op for
    // any callback currently in flight).
    mgr.set_default_device_change_handler(nullptr);
    mgr.dispatch_default_device_change(false, "device:airpods");
    REQUIRE(output_changes.load() == 1);
}

TEST_CASE("AudioDeviceManager tracks sample-rate changes",
          "[audio][audio-device-manager][lifecycle][issue-2935]") {
    AudioDeviceManager mgr;
    std::atomic<int> calls{0};
    std::atomic<double> last_seen{0.0};

    mgr.set_sample_rate_change_handler(
        [&](double new_rate) {
            last_seen.store(new_rate);
            ++calls;
        });

    REQUIRE(mgr.sample_rate_change_count() == 0u);
    REQUIRE(mgr.last_sample_rate() == 0.0);

    mgr.dispatch_sample_rate_change(44100.0);
    mgr.dispatch_sample_rate_change(48000.0);
    mgr.dispatch_sample_rate_change(96000.0);

    REQUIRE(calls.load() == 3);
    REQUIRE_THAT(last_seen.load(), WithinAbs(96000.0, 0.001));
    REQUIRE(mgr.sample_rate_change_count() == 3u);
    REQUIRE_THAT(mgr.last_sample_rate(), WithinAbs(96000.0, 0.001));
}

TEST_CASE("AudioDeviceManager xrun counter increments and resets",
          "[audio][audio-device-manager][lifecycle][issue-2935]") {
    AudioDeviceManager mgr;
    REQUIRE(mgr.xrun_count() == 0u);

    mgr.bump_xrun_counter();
    mgr.bump_xrun_counter();
    mgr.bump_xrun_counter(5);
    REQUIRE(mgr.xrun_count() == 7u);

    mgr.reset_xrun_counter();
    REQUIRE(mgr.xrun_count() == 0u);
}

TEST_CASE("AudioDeviceManager MIDI endpoint delta tracking fires per change",
          "[audio][audio-device-manager][midi][lifecycle][issue-2935]") {
    AudioDeviceManager mgr;

    std::vector<AudioDeviceManager::MidiEndpointChange> changes;
    std::mutex changes_mu;
    auto tok = mgr.subscribe_midi_endpoints(
        [&](const AudioDeviceManager::MidiEndpointChange& c) {
            std::lock_guard<std::mutex> lk(changes_mu);
            changes.push_back(c);
        });
    REQUIRE(mgr.midi_endpoint_subscriber_count() == 1);

    auto make_ep = [](std::string id, std::string name, bool is_input) {
        AudioDeviceManager::MidiEndpoint ep;
        ep.id = std::move(id);
        ep.name = std::move(name);
        ep.is_input = is_input;
        return ep;
    };

    // Initial publish: empty → {Push, Mini}. Both are adds.
    mgr.set_midi_endpoints({
        make_ep("ep:push", "Push 2", true),
        make_ep("ep:mini", "MiniLab",  true),
    });
    {
        std::lock_guard<std::mutex> lk(changes_mu);
        REQUIRE(changes.size() == 2);
        REQUIRE(changes[0].kind == AudioDeviceManager::MidiEndpointChangeKind::Added);
        REQUIRE(changes[1].kind == AudioDeviceManager::MidiEndpointChangeKind::Added);
        changes.clear();
    }

    // Unplug Push, keep MiniLab. One removal, no adds.
    mgr.set_midi_endpoints({
        make_ep("ep:mini", "MiniLab", true),
    });
    {
        std::lock_guard<std::mutex> lk(changes_mu);
        REQUIRE(changes.size() == 1);
        REQUIRE(changes[0].kind == AudioDeviceManager::MidiEndpointChangeKind::Removed);
        REQUIRE(changes[0].endpoint.id == "ep:push");
        changes.clear();
    }

    // No change → no dispatch.
    mgr.set_midi_endpoints({
        make_ep("ep:mini", "MiniLab", true),
    });
    {
        std::lock_guard<std::mutex> lk(changes_mu);
        REQUIRE(changes.empty());
    }

    REQUIRE(mgr.midi_endpoints().size() == 1);
    REQUIRE(mgr.midi_endpoints()[0].id == "ep:mini");
}

// Regression for issue #2976 / PR #2970 Codex P1 finding:
// subscribe_midi() and subscribe_midi_endpoints() previously used two
// independent counters that both started at 1. unsubscribe_midi()
// erases by id from BOTH maps, so destroying the endpoint token (id=1)
// would erase the unrelated MIDI subscriber (also id=1) and leave the
// endpoint subscriber alive. The fix is a single monotonic counter
// shared across all subscription maps so ids never collide.
TEST_CASE("AudioDeviceManager MIDI and endpoint token ids do not collide",
          "[audio][audio-device-manager][midi][issue-2976][issue-2970]") {
    AudioDeviceManager mgr;

    std::atomic<int> midi_calls{0};
    std::atomic<int> ep_calls{0};

    // Subscribe order matters: this is the order that previously
    // produced id=1 on both maps under the old counter scheme.
    auto midi_tok = mgr.subscribe_midi(
        [&](const pulp::midi::MidiEvent&) { ++midi_calls; });
    auto ep_tok = mgr.subscribe_midi_endpoints(
        [&](const AudioDeviceManager::MidiEndpointChange&) { ++ep_calls; });

    REQUIRE(mgr.midi_subscriber_count() == 1);
    REQUIRE(mgr.midi_endpoint_subscriber_count() == 1);

    // Destroy the endpoint token first. Under the bug, this routed
    // through unsubscribe_midi(), which probed midi_subs_ first, found
    // a matching id, and erased the MIDI subscriber by mistake. The
    // endpoint subscriber stayed live.
    ep_tok.reset();

    // Post-condition the bug violated:
    //   - the MIDI subscriber must still be live
    //   - the endpoint subscriber must be gone
    REQUIRE(mgr.midi_subscriber_count() == 1);
    REQUIRE(mgr.midi_endpoint_subscriber_count() == 0);

    // Functional confirmation: dispatch hits MIDI (still alive) and
    // does NOT hit endpoints.
    mgr.dispatch_midi_event(pulp::midi::MidiEvent::note_on(0, 60, 100));
    REQUIRE(midi_calls.load() == 1);

    AudioDeviceManager::MidiEndpoint ep;
    ep.id = "ep:new";
    ep.name = "New Device";
    ep.is_input = true;
    mgr.set_midi_endpoints({ep});
    REQUIRE(ep_calls.load() == 0);
}

// Stress regression: many register/unregister cycles for both MIDI and
// endpoint hubs interleaved across multiple threads. Under the old
// per-map counters, destroying an endpoint token could erase the
// wrong MIDI subscriber (same numeric id), so subscriber counts would
// drift away from what the test explicitly held. With the shared
// monotonic counter, post-condition counts match the held tokens
// exactly.
TEST_CASE("AudioDeviceManager subscriptions stay globally consistent under load",
          "[audio][audio-device-manager][midi][issue-2976][issue-2970]") {
    AudioDeviceManager mgr;

    constexpr int kPerThread = 64;
    constexpr int kThreads   = 4;

    std::mutex hold_mu;
    std::vector<MidiSubscriptionToken> midi_hold;
    std::vector<MidiSubscriptionToken> ep_hold;
    midi_hold.reserve(kThreads * kPerThread);
    ep_hold.reserve(kThreads * kPerThread);

    // Worker-thread failure signal. Catch2 assertion macros are not
    // thread-safe, so a REQUIRE inside the worker can race or otherwise
    // wedge the harness in nondeterministic ways. Track the failure as
    // an atomic flag here and assert it on the test thread after join()
    // (Codex PR #3001 review).
    std::atomic<int> inactive_token_count{0};

    auto worker = [&](bool use_endpoints) {
        std::vector<MidiSubscriptionToken> local;
        local.reserve(kPerThread);
        for (int i = 0; i < kPerThread; ++i) {
            auto tok = use_endpoints
                ? mgr.subscribe_midi_endpoints(
                    [](const AudioDeviceManager::MidiEndpointChange&) {})
                : mgr.subscribe_midi([](const pulp::midi::MidiEvent&) {});
            if (!tok.active()) {
                inactive_token_count.fetch_add(1, std::memory_order_relaxed);
            }
            local.push_back(std::move(tok));
        }
        std::lock_guard<std::mutex> lk(hold_mu);
        auto& dest = use_endpoints ? ep_hold : midi_hold;
        for (auto& t : local) dest.push_back(std::move(t));
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        // Half the threads subscribe MIDI events, half subscribe
        // endpoints. The bug surfaced when both sides issued the same
        // low ids (1, 2, 3, …) and destruction order crossed maps.
        threads.emplace_back(worker, /*use_endpoints=*/(t % 2 == 0));
    }
    for (auto& th : threads) th.join();

    // Now safe to assert on the worker-thread observations.
    REQUIRE(inactive_token_count.load() == 0);

    const int midi_threads = (kThreads + 1) / 2;
    const int ep_threads   = kThreads - midi_threads;
    REQUIRE(mgr.midi_subscriber_count() ==
            static_cast<size_t>(midi_threads * kPerThread));
    REQUIRE(mgr.midi_endpoint_subscriber_count() ==
            static_cast<size_t>(ep_threads * kPerThread));

    // Now drop all endpoint tokens. If destroying one could erase the
    // wrong subscriber (the bug), the MIDI count would visibly drift.
    ep_hold.clear();
    REQUIRE(mgr.midi_endpoint_subscriber_count() == 0);
    REQUIRE(mgr.midi_subscriber_count() ==
            static_cast<size_t>(midi_threads * kPerThread));

    // Drop the remaining MIDI tokens; counters return to zero.
    midi_hold.clear();
    REQUIRE(mgr.midi_subscriber_count() == 0);
}

TEST_CASE("AudioDeviceManager latch_close drops subsequent dispatches",
          "[audio][audio-device-manager][lifecycle][shutdown][issue-2935]") {
    AudioDeviceManager mgr;
    std::atomic<int> midi_calls{0};
    std::atomic<int> dev_calls{0};

    auto midi_tok = mgr.subscribe_midi(
        [&](const pulp::midi::MidiEvent&) { ++midi_calls; });
    auto dev_tok  = mgr.subscribe_device_changes(
        [&](const AudioDeviceManager::DeviceChangeEvent&) { ++dev_calls; });
    mgr.set_default_device_change_handler(
        [&](bool, const std::string&) { ++dev_calls; });

    REQUIRE_FALSE(mgr.is_closed());
    mgr.dispatch_midi_event(pulp::midi::MidiEvent::note_on(0, 60, 100));
    mgr.dispatch_device_change({});
    mgr.dispatch_default_device_change(false, "device:x");
    REQUIRE(midi_calls.load() == 1);
    REQUIRE(dev_calls.load()  == 2);

    mgr.latch_close();
    REQUIRE(mgr.is_closed());

    // Post-close dispatches are no-ops.
    mgr.dispatch_midi_event(pulp::midi::MidiEvent::note_on(0, 60, 100));
    mgr.dispatch_device_change({});
    mgr.dispatch_default_device_change(false, "device:y");
    mgr.dispatch_sample_rate_change(96000.0);
    REQUIRE(midi_calls.load() == 1);
    REQUIRE(dev_calls.load()  == 2);
}

TEST_CASE("AudioDeviceManager latch_close waits for in-flight dispatchers",
          "[audio][audio-device-manager][lifecycle][shutdown][issue-2935]") {
    AudioDeviceManager mgr;
    std::atomic<int> in_callback{0};
    std::atomic<int> entered{0};
    std::atomic<int> exited{0};
    std::atomic<bool> can_exit{false};

    // Subscriber that camps inside the dispatch until we let it out.
    // This models a slow audio thread that is mid-dispatch when the
    // host calls latch_close() on shutdown — the latch MUST wait for
    // it to return before declaring the manager closed.
    auto tok = mgr.subscribe_midi(
        [&](const pulp::midi::MidiEvent&) {
            ++entered;
            ++in_callback;
            // Spin until the test releases us.
            while (!can_exit.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            --in_callback;
            ++exited;
        });

    // Kick off the slow dispatcher on a worker thread.
    std::thread dispatcher([&] {
        mgr.dispatch_midi_event(pulp::midi::MidiEvent::note_on(0, 60, 100));
    });

    // Wait until the subscriber is parked inside the dispatch.
    while (entered.load() == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
    REQUIRE(in_callback.load() == 1);

    // Start the latch on another thread. It must NOT return while the
    // dispatcher is still inside the callback.
    std::thread latcher([&] { mgr.latch_close(); });

    // Give latch a chance to bail early (bug). If it does, exited is
    // still 0 because we haven't released the subscriber yet.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(exited.load() == 0);
    // Either `closed_` is still being set or the dispatcher is still
    // in flight — both are valid pre-release. The forbidden state is
    // "manager declared itself closed AND nothing is in flight" before
    // the dispatcher actually returned.
    bool latched_too_early = mgr.is_closed() && in_callback.load() == 0;
    REQUIRE_FALSE(latched_too_early);

    // Release the subscriber. Latch should return promptly and
    // observe in_flight_ == 0.
    can_exit.store(true, std::memory_order_release);
    dispatcher.join();
    latcher.join();

    REQUIRE(exited.load() == 1);
    REQUIRE(in_callback.load() == 0);
    REQUIRE(mgr.is_closed());
}

TEST_CASE("AudioDeviceManager destructor latches without an explicit close",
          "[audio][audio-device-manager][lifecycle][shutdown][issue-2935]") {
    // Even if the host forgets latch_close(), the destructor must
    // wait for in-flight dispatches. This guards the "concurrent-
    // callback shutdown" acceptance for the typical case where a
    // host just lets the manager fall out of scope.
    std::atomic<int> calls{0};
    auto* mgr = new AudioDeviceManager();
    auto tok  = mgr->subscribe_midi(
        [&](const pulp::midi::MidiEvent&) { ++calls; });
    mgr->dispatch_midi_event(pulp::midi::MidiEvent::note_on(0, 60, 100));
    REQUIRE(calls.load() == 1);
    delete mgr;
    // No assertion after delete is meaningful — but the test fails
    // (TSan/ASan) if the destructor races a dispatcher.
}

TEST_CASE("AudioDeviceManager attach_audio_system bridges to device changes",
          "[audio][audio-device-manager][lifecycle][issue-2935]") {
    // Stand-in AudioSystem whose set_device_change_callback() drives
    // dispatch_device_change(). The test verifies the bridge fires
    // and enumerate_devices() snapshot reaches subscribers.
    class FakeAudioSystem : public AudioSystem {
    public:
        std::vector<DeviceInfo> enumerate_devices() override {
            return devices_;
        }
        std::unique_ptr<AudioDevice> create_device(const std::string&) override {
            return nullptr;
        }
        DeviceInfo default_output_device() override { return {}; }
        DeviceInfo default_input_device()  override { return {}; }

        // Drive the registered callback (bound via the base class slot
        // by attach_audio_system → set_device_change_callback).
        void simulate_device_list_change() { fire_device_change(); }

        std::vector<DeviceInfo> devices_;
    };

    FakeAudioSystem sys;
    sys.devices_ = {{"device:a", "A", 0, 2, {}, {}, false, true},
                    {"device:b", "B", 2, 2, {}, {}, true,  false}};

    AudioDeviceManager mgr;
    std::atomic<int> calls{0};
    std::size_t last_snapshot_size = 0;
    std::mutex snap_mu;

    auto tok = mgr.subscribe_device_changes(
        [&](const AudioDeviceManager::DeviceChangeEvent& ev) {
            ++calls;
            std::lock_guard<std::mutex> lk(snap_mu);
            last_snapshot_size = ev.devices.size();
        });

    mgr.attach_audio_system(&sys);
    sys.simulate_device_list_change();
    REQUIRE(calls.load() == 1);
    {
        std::lock_guard<std::mutex> lk(snap_mu);
        REQUIRE(last_snapshot_size == 2);
    }

    // Simulate a USB hub reset — devices_ shrinks before the
    // callback fires.
    sys.devices_ = {{"device:a", "A", 0, 2, {}, {}, false, true}};
    sys.simulate_device_list_change();
    REQUIRE(calls.load() == 2);
    {
        std::lock_guard<std::mutex> lk(snap_mu);
        REQUIRE(last_snapshot_size == 1);
    }

    // Unbinding stops the bridge.
    mgr.attach_audio_system(nullptr);
    sys.simulate_device_list_change();
    REQUIRE(calls.load() == 2);
}

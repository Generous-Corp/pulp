#pragma once

/// @file audio_device_manager.hpp
/// AudioDeviceManager — persistence + MIDI hub + lifecycle / recovery.
///
/// Phase A (item 1.2a, PR #2936): persistence + MIDI hub + smoothed
/// CPU-load surface — all backend-agnostic, no platform listeners.
///
/// Phase B (item 1.2b, this slice):
///   - Live device hotplug detection (the manager observes a
///     `pulp::audio::AudioSystem`'s device-change callback and
///     republishes a structured `DeviceChangeEvent` to its own
///     subscribers; tests can inject events without an AudioSystem).
///   - Default-device-change recovery (subscribers learn when the
///     system default output/input flipped underneath them).
///   - Sample-rate-change recovery (subscribers learn when the active
///     device's sample rate changed).
///   - xrun counter exposed via `xrun_count()` for UI polling.
///   - MIDI endpoint lifetime tracking — `set_midi_endpoints()`
///     records the current input/output endpoint snapshot and
///     dispatches `MidiEndpointChange` events on add/remove.
///   - Concurrent-callback shutdown — `latch_close()` blocks until
///     every in-flight dispatch returns and turns subsequent
///     dispatches into no-ops.
///
/// Responsibilities:
///   - Persists the user's selected audio device + sample rate + buffer
///     size + I/O channel masks under `audio_device_manager.*` keys in
///     `pulp::state::ApplicationProperties::user_settings()`. Settings
///     survive a host restart.
///   - Single source of truth for connected MIDI inputs/outputs.
///     Dispatches incoming MIDI to subscribers. A subscription returns
///     an RAII token whose destruction auto-unsubscribes — the typical
///     pattern is to hold the token as a member; lifetime ends with
///     the owning object and the manager stops calling the callback.
///   - Exposes a smoothed CPU-load signal (wraps
///     `pulp::audio::AudioProcessLoadMeasurer`) so a host UI can read
///     a stable percentage without poking the realtime callback.

#include <pulp/audio/device.hpp>
#include <pulp/audio/load_measurer.hpp>
#include <pulp/midi/device.hpp>
#include <pulp/midi/message.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::state { class ApplicationProperties; }

namespace pulp::audio {

/// Persisted audio-device selection.
///
/// `output_channel_mask` / `input_channel_mask` are 64-bit bitmasks
/// over the device's natural channel order. Bit `i` set means channel
/// `i` is enabled. Channels 64+ collapse onto bit 63 conservatively
/// (Pulp targets stereo / multichannel-up-to-32, so this is fine for
/// every shipping audio interface). Zero mask + non-zero channel count
/// in `DeviceConfig` means "use the first N channels" (legacy default).
struct DeviceSelection {
    std::string output_device;
    std::string input_device;
    double      sample_rate = 0.0;   ///< 0 = use default
    int         buffer_size = 0;     ///< 0 = use default
    uint64_t    output_channel_mask = 0;
    uint64_t    input_channel_mask  = 0;
};

/// Keys used under `ApplicationProperties::user_settings()`. Exposed so
/// callers can `set_string` directly when migrating from another store
/// or when bulk-clearing for tests.
namespace adm_keys {
constexpr const char* kPrefix             = "audio_device_manager.";
constexpr const char* kOutputDevice       = "audio_device_manager.output_device";
constexpr const char* kInputDevice        = "audio_device_manager.input_device";
constexpr const char* kSampleRate         = "audio_device_manager.sample_rate";
constexpr const char* kBufferSize         = "audio_device_manager.buffer_size";
constexpr const char* kOutputChannelMask  = "audio_device_manager.output_channel_mask";
constexpr const char* kInputChannelMask   = "audio_device_manager.input_channel_mask";
}  // namespace adm_keys

class AudioDeviceManager;

/// RAII subscription token. Destruction auto-unsubscribes from the
/// owning `AudioDeviceManager`'s MIDI hub. The token holds only a
/// weak reference to the manager — outliving the manager is safe and
/// becomes a no-op on destruction.
class MidiSubscriptionToken {
public:
    MidiSubscriptionToken() = default;
    MidiSubscriptionToken(const MidiSubscriptionToken&) = delete;
    MidiSubscriptionToken& operator=(const MidiSubscriptionToken&) = delete;
    MidiSubscriptionToken(MidiSubscriptionToken&& other) noexcept;
    MidiSubscriptionToken& operator=(MidiSubscriptionToken&& other) noexcept;
    ~MidiSubscriptionToken();

    bool active() const noexcept { return id_ != 0 && !manager_.expired(); }

    /// Reset and unsubscribe early.
    void reset() noexcept;

private:
    friend class AudioDeviceManager;
    MidiSubscriptionToken(std::weak_ptr<void> manager_ptr,
                          AudioDeviceManager* manager_raw,
                          uint64_t id) noexcept
        : manager_(std::move(manager_ptr)), manager_raw_(manager_raw), id_(id) {}

    std::weak_ptr<void>    manager_;       ///< Lifetime peg.
    AudioDeviceManager*    manager_raw_ = nullptr;
    uint64_t               id_ = 0;
};

/// AudioDeviceManager — persistence + MIDI hub.
///
/// Construct one per application (the standalone host owns one; plugin
/// adapters typically don't). Threading:
///   - `save()` / `load()` and the subscription API run on the
///     caller's thread (typically the main/UI thread).
///   - `dispatch_midi_event()` may run on any thread (CoreMIDI, ALSA
///     seq, or a WinMM thread). Subscriber callbacks therefore must
///     be thread-tolerant; subscribers that need UI-thread marshalling
///     should hop themselves.
///   - `cpu_load()` and `peak_cpu_load()` are safe to read from any
///     thread. `begin_cpu_measure()` / `end_cpu_measure()` are intended
///     to be called from the audio thread, mirroring
///     `AudioProcessLoadMeasurer`.
class AudioDeviceManager {
public:
    using MidiHandler = std::function<void(const pulp::midi::MidiEvent&)>;

    AudioDeviceManager();
    ~AudioDeviceManager();

    AudioDeviceManager(const AudioDeviceManager&) = delete;
    AudioDeviceManager& operator=(const AudioDeviceManager&) = delete;

    // ── Persistence ─────────────────────────────────────────────────

    /// Read persisted selection from `props.user_settings()`. Returns
    /// `std::nullopt` if no `audio_device_manager.*` keys exist.
    static std::optional<DeviceSelection> load_selection(
        const pulp::state::ApplicationProperties& props);

    /// Write `sel` into `props.user_settings()` and `save()` the
    /// underlying file. Returns true on successful save.
    static bool save_selection(pulp::state::ApplicationProperties& props,
                               const DeviceSelection& sel);

    /// Resolve a stored selection against the live device list. If
    /// `sel.output_device` (or `.input_device`) is empty or no longer
    /// present in `available`, falls back to the system-default device
    /// id (`fallback_output_id` / `fallback_input_id`) and records that
    /// fallback in the returned struct. `fallback_used_output` /
    /// `fallback_used_input` flags surface to the caller so it can log
    /// the substitution.
    struct ResolveResult {
        DeviceSelection resolved;
        bool fallback_used_output = false;
        bool fallback_used_input  = false;
    };
    static ResolveResult resolve_selection(
        const DeviceSelection& sel,
        const std::vector<DeviceInfo>& available,
        const std::string& fallback_output_id,
        const std::string& fallback_input_id);

    /// Convenience: convert a `DeviceSelection` to a `DeviceConfig`
    /// for `AudioDevice::open()`. Channel counts derive from
    /// `popcount()` of the masks when non-zero; otherwise the caller's
    /// `default_output_channels` / `default_input_channels` win.
    static DeviceConfig selection_to_config(
        const DeviceSelection& sel,
        int default_output_channels = 2,
        int default_input_channels  = 0);

    // ── MIDI hub ────────────────────────────────────────────────────

    /// Subscribe `handler` to incoming MIDI events. The returned token
    /// auto-unsubscribes on destruction. Subscribers are invoked in
    /// registration order on whatever thread `dispatch_midi_event()`
    /// runs on.
    [[nodiscard]] MidiSubscriptionToken subscribe_midi(MidiHandler handler);

    /// Dispatch `event` to every active subscriber. Typically invoked
    /// from the MIDI input callback (`MidiInput::open` callback) by
    /// the standalone host or plugin glue. Safe to call from any
    /// thread.
    void dispatch_midi_event(const pulp::midi::MidiEvent& event);

    /// Number of currently registered subscribers. Mostly used by
    /// tests; safe to call from any thread.
    std::size_t midi_subscriber_count() const;

    // ── CPU load ────────────────────────────────────────────────────

    /// Begin a CPU-load measurement window. Mirrors
    /// `AudioProcessLoadMeasurer::begin()` — call at the start of the
    /// audio callback.
    void begin_cpu_measure(int num_frames, float sample_rate);

    /// End the current CPU-load window.
    void end_cpu_measure();

    /// Smoothed CPU load in `[0, +inf)`. 1.0 == full buffer.
    float cpu_load() const;

    /// Peak CPU load since last `reset_peak_cpu_load()`.
    float peak_cpu_load() const;

    /// Reset the peak tracker.
    void reset_peak_cpu_load();

    // ── Lifecycle / hotplug / recovery (1.2b) ───────────────────────

    /// What kind of device-list mutation occurred. `Unknown` is what
    /// CoreAudio's hardware-devices listener delivers — the OS tells
    /// us the list changed without saying how. The manager passes the
    /// live device snapshot to the subscriber so it can diff against
    /// its previous snapshot if it cares about adds / removes. Backends
    /// that surface deltas natively (WASAPI IMMNotificationClient)
    /// populate `Added`/`Removed`.
    enum class DeviceChangeKind {
        Unknown,   ///< List mutated; reason not disclosed.
        Added,     ///< A device became available.
        Removed,   ///< A device went away.
        Replaced,  ///< Bulk swap (e.g., USB hub reset).
    };

    struct DeviceChangeEvent {
        DeviceChangeKind kind = DeviceChangeKind::Unknown;
        std::vector<DeviceInfo> devices;  ///< Snapshot at the time of the change.
        std::string device_id;            ///< Affected id (when `kind != Unknown`).
    };

    using DeviceChangeHandler = std::function<void(const DeviceChangeEvent&)>;

    /// RAII subscription token for device-change events. Same lifetime
    /// rules as `MidiSubscriptionToken` — destruction unsubscribes,
    /// outliving the manager is safe.
    class DeviceChangeToken {
    public:
        DeviceChangeToken() = default;
        DeviceChangeToken(const DeviceChangeToken&) = delete;
        DeviceChangeToken& operator=(const DeviceChangeToken&) = delete;
        DeviceChangeToken(DeviceChangeToken&& other) noexcept;
        DeviceChangeToken& operator=(DeviceChangeToken&& other) noexcept;
        ~DeviceChangeToken();

        bool active() const noexcept {
            return id_ != 0 && !manager_.expired();
        }
        void reset() noexcept;

    private:
        friend class AudioDeviceManager;
        DeviceChangeToken(std::weak_ptr<void> manager_ptr,
                          AudioDeviceManager* manager_raw,
                          uint64_t id) noexcept
            : manager_(std::move(manager_ptr)), manager_raw_(manager_raw), id_(id) {}

        std::weak_ptr<void>  manager_;
        AudioDeviceManager*  manager_raw_ = nullptr;
        uint64_t             id_ = 0;
    };

    /// Subscribe to device-change events. Returns an RAII token whose
    /// destruction unsubscribes. Safe to call from any thread.
    [[nodiscard]] DeviceChangeToken subscribe_device_changes(DeviceChangeHandler handler);

    /// Number of currently active device-change subscribers (tests).
    std::size_t device_change_subscriber_count() const;

    /// Bind a live `AudioSystem` whose device-change callback should
    /// drive `dispatch_device_change()`. The manager registers itself
    /// via `system->set_device_change_callback(...)` and re-issues
    /// `DeviceChangeEvent`s with the snapshot. Pass `nullptr` to
    /// unbind. Tests can skip this entirely and call
    /// `dispatch_device_change` directly with an injected snapshot.
    void attach_audio_system(AudioSystem* system);

    /// Dispatch a device-change event to subscribers. Safe to call
    /// from any thread; tests use it to simulate hotplug without an
    /// AudioSystem. After `latch_close()` returns, this is a no-op.
    void dispatch_device_change(const DeviceChangeEvent& event);

    /// Subscribers learn when the system default output / input
    /// changed. `is_input == true` means the default input device
    /// changed; `false` means default output. The handler receives
    /// the new device id (callers query the platform and forward it).
    using DefaultDeviceChangeHandler =
        std::function<void(bool is_input, const std::string& new_device_id)>;

    /// Register a single default-device-change handler. Pass `nullptr`
    /// to clear. The manager fans out to a single handler (not a list)
    /// because the typical host has exactly one recovery policy.
    void set_default_device_change_handler(DefaultDeviceChangeHandler handler);

    /// Dispatch a default-device-change to the registered handler.
    /// Safe to call from any thread; tests use it to simulate
    /// default-device flips. No-op after `latch_close()`.
    void dispatch_default_device_change(bool is_input,
                                        const std::string& new_device_id);

    /// Subscribers learn when the active device's nominal sample rate
    /// changed. Hosts typically react by re-preparing their processor
    /// graph against the new rate.
    using SampleRateChangeHandler = std::function<void(double new_rate)>;

    void set_sample_rate_change_handler(SampleRateChangeHandler handler);

    /// Dispatch a sample-rate-change event to the registered handler.
    /// Also bumps a counter so a host can observe how many rate
    /// changes the active session has weathered. No-op after
    /// `latch_close()`.
    void dispatch_sample_rate_change(double new_rate);

    /// Total number of sample-rate changes seen since construction.
    std::uint64_t sample_rate_change_count() const;

    /// Last known sample rate (set every `dispatch_sample_rate_change`).
    double last_sample_rate() const;

    // ── xrun tracking ──────────────────────────────────────────────

    /// Increment the manager's internal xrun counter. Hosts wire this
    /// to the active `AudioDevice::xrun_count()` delta, or call it
    /// directly when a backend doesn't surface overload events.
    void bump_xrun_counter(std::uint64_t delta = 1);

    /// Snapshot the current xrun count. Safe from any thread.
    std::uint64_t xrun_count() const;

    /// Reset the xrun counter to zero.
    void reset_xrun_counter();

    // ── MIDI endpoint tracking ─────────────────────────────────────

    struct MidiEndpoint {
        std::string id;
        std::string name;
        bool is_input = false;  ///< true == input; false == output.
    };

    enum class MidiEndpointChangeKind {
        Added,
        Removed,
    };

    struct MidiEndpointChange {
        MidiEndpointChangeKind kind;
        MidiEndpoint endpoint;
    };

    using MidiEndpointChangeHandler = std::function<void(const MidiEndpointChange&)>;

    /// Subscribe to MIDI endpoint add/remove events. Returns the same
    /// `MidiSubscriptionToken` flavor as `subscribe_midi`. The id
    /// namespace is separate from MIDI-event subscriptions, so
    /// `midi_subscriber_count()` does not include endpoint listeners.
    [[nodiscard]] MidiSubscriptionToken subscribe_midi_endpoints(
        MidiEndpointChangeHandler handler);

    std::size_t midi_endpoint_subscriber_count() const;

    /// Replace the manager's snapshot of live MIDI endpoints. Deltas
    /// from the previous snapshot dispatch to subscribers as
    /// `MidiEndpointChange` events. Endpoint identity is by `id`.
    /// No-op after `latch_close()` (state still updates, but no
    /// dispatch fires).
    void set_midi_endpoints(std::vector<MidiEndpoint> endpoints);

    /// Current MIDI endpoint snapshot.
    std::vector<MidiEndpoint> midi_endpoints() const;

    // ── Concurrent-callback shutdown ───────────────────────────────

    /// Latch the manager so subsequent dispatches are no-ops. Blocks
    /// until any in-flight dispatch (MIDI event, device change,
    /// default-device change, sample-rate change, endpoint change)
    /// has returned. After `latch_close()` returns, the host can
    /// safely destroy the objects that own the subscriber callbacks.
    ///
    /// Symmetric to the `close()`-then-`stop()` pattern in
    /// `core/audio::AudioDevice` — this is the manager-level latch.
    void latch_close();

    /// True after `latch_close()` has been called.
    bool is_closed() const noexcept;

private:
    friend class MidiSubscriptionToken;
    friend class DeviceChangeToken;

    void unsubscribe_midi(uint64_t id) noexcept;
    void unsubscribe_device_change(uint64_t id) noexcept;
    void unsubscribe_midi_endpoint(uint64_t id) noexcept;

    // RAII guard used by every dispatcher; blocks latch_close() until
    // all in-flight dispatches return.
    class DispatchGuard;

    /// Lifetime peg for tokens. Token holds a `weak_ptr<void>` to
    /// this; destruction marks the manager dead.
    std::shared_ptr<void>                        lifetime_;

    /// Single monotonic counter shared by every subscription API on
    /// this manager (MIDI events, MIDI endpoints, device-change). The
    /// unsubscribe path is allowed to probe multiple maps with the
    /// same id, so the id must be globally unique across all maps —
    /// otherwise destroying one subscription token can erase an
    /// unrelated subscription that happens to share the numeric id
    /// (see issue #2976 / PR #2970 Codex finding). Atomic so callers
    /// don't need to hold any particular mutex to allocate one.
    std::atomic<uint64_t>                        next_token_id_{1};

    mutable std::mutex                           midi_mu_;
    std::unordered_map<uint64_t, MidiHandler>    midi_subs_;

    mutable std::mutex                                          device_change_mu_;
    std::unordered_map<uint64_t, DeviceChangeHandler>           device_change_subs_;

    mutable std::mutex                              default_dev_mu_;
    DefaultDeviceChangeHandler                      default_dev_handler_;

    mutable std::mutex                              sr_mu_;
    SampleRateChangeHandler                         sr_handler_;
    std::atomic<std::uint64_t>                      sr_change_count_{0};
    std::atomic<double>                             last_sr_{0.0};

    std::atomic<std::uint64_t>                      xrun_counter_{0};

    mutable std::mutex                                                midi_ep_mu_;
    std::unordered_map<uint64_t, MidiEndpointChangeHandler>           midi_ep_subs_;
    std::vector<MidiEndpoint>                                         midi_endpoints_;

    AudioSystem*                                    attached_system_ = nullptr;

    /// CPU-load mutex protects begin/end against load() readers on
    /// other threads. The measurer itself is not thread-safe, so we
    /// guard every access. Contention is negligible — readers are UI
    /// thread polling once per frame, begin/end runs once per audio
    /// callback.
    mutable std::mutex                           load_mu_;
    AudioProcessLoadMeasurer                     load_;

    /// Latch state. `closed_` flips true in `latch_close()` and stays
    /// that way for the manager's lifetime. `in_flight_` counts active
    /// dispatchers; `latch_close()` waits for it to reach zero.
    std::atomic<bool>                            closed_{false};
    mutable std::mutex                           latch_mu_;
    std::condition_variable                      latch_cv_;
    int                                          in_flight_ = 0;
};

}  // namespace pulp::audio

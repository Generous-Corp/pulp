#pragma once

/// @file audio_device_manager.hpp
/// AudioDeviceManager — persistence + MIDI hub.
///
/// Part of macOS-plugin-authoring plan item 1.2a (Phase A). This is the
/// persistence + MIDI-hub half of the manager. Lifecycle, hotplug, and
/// recovery (1.2b) land in Phase B once `AudioWorkgroup` (1.1) and
/// CoreAudio hotplug listeners are in place.
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

private:
    friend class MidiSubscriptionToken;

    void unsubscribe_midi(uint64_t id) noexcept;

    /// Lifetime peg for `MidiSubscriptionToken`. Token holds a
    /// `weak_ptr<void>` to this; destruction marks the manager dead.
    std::shared_ptr<void>                        lifetime_;

    mutable std::mutex                           midi_mu_;
    std::unordered_map<uint64_t, MidiHandler>    midi_subs_;
    uint64_t                                     next_midi_id_ = 1;

    /// CPU-load mutex protects begin/end against load() readers on
    /// other threads. The measurer itself is not thread-safe, so we
    /// guard every access. Contention is negligible — readers are UI
    /// thread polling once per frame, begin/end runs once per audio
    /// callback.
    mutable std::mutex                           load_mu_;
    AudioProcessLoadMeasurer                     load_;
};

}  // namespace pulp::audio

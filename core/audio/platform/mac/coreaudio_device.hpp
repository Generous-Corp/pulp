#pragma once

#include "coreaudio_io_timing.hpp"
#include <pulp/audio/device.hpp>
#include <pulp/audio/workgroup.hpp>
#include <AudioToolbox/AudioToolbox.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <memory>
#include <optional>

#if defined(__APPLE__)
#include <os/workgroup.h>
#endif

namespace pulp::audio::mac {

#if defined(__APPLE__)
/// A failed AudioOutputUnitStop does not prove that the old render callback has
/// drained. The caller must leave CurrentDevice and its retained workgroup
/// reference untouched in that case.
constexpr bool coreaudio_stop_allows_device_switch(OSStatus status) noexcept {
    return status == noErr;
}

/// A live-device retarget is running only after the replacement AudioUnit
/// restart succeeds. The listener thread owns `running` while holding the
/// device switch mutex.
constexpr bool update_coreaudio_running_after_restart(
    OSStatus status, bool& running) noexcept {
    running = status == noErr;
    return running;
}

template <typename ConfigureFn>
bool configure_coreaudio_fallback_priority_once(
    std::atomic<bool>& configured, ConfigureFn&& configure) noexcept {
    if (configured.load(std::memory_order_acquire)) return true;
    if (!configure()) return false;
    configured.store(true, std::memory_order_release);
    return true;
}

/// Owning slot for the caller-retained value returned by
/// kAudioDevicePropertyIOThreadOSWorkgroup. `adopt()` consumes one query
/// reference; replacement, reset, and destruction release it exactly once.
/// The injectable release operation is a narrow ownership-balance test seam.
class CoreAudioWorkgroupReference {
public:
    using ReleaseFn = void (*)(os_workgroup_t) noexcept;

    explicit CoreAudioWorkgroupReference(
        ReleaseFn release = release_os_workgroup) noexcept
        : release_(release) {}
    ~CoreAudioWorkgroupReference() { reset(); }

    CoreAudioWorkgroupReference(const CoreAudioWorkgroupReference&) = delete;
    CoreAudioWorkgroupReference& operator=(const CoreAudioWorkgroupReference&) = delete;

    void adopt(os_workgroup_t value) noexcept {
        auto* const old = handle_;
        handle_ = value;
        if (old) release_(old);
    }
    void reset() noexcept { adopt(nullptr); }
    os_workgroup_t get() const noexcept { return handle_; }

private:
    static void release_os_workgroup(os_workgroup_t value) noexcept;

    os_workgroup_t handle_ = nullptr;
    ReleaseFn release_;
};
#endif

/// Frames of input that may safely be requested, given what was allocated.
///
/// A free function, and PUBLIC, for one reason: the render callback is a
/// static member driven by CoreAudio with a live AudioUnit, so the only thing
/// a headless test can reach is this. Keeping the rule in one named place
/// means the test exercises the code the callback runs rather than a copy of
/// its reasoning — a test that re-implements a clamp passes while the clamp is
/// missing.
///
/// The bug it exists to prevent: the callback declared `inNumberFrames` of
/// space to AudioUnitRender and requested that many, while the storage is
/// allocated once for the configured block size. A device asking for more had
/// CoreAudio memcpy past the end of the heap buffer, on the audio thread —
/// found by AddressSanitizer as a 1880-byte heap-buffer-overflow, and observed
/// only as an abort later, in whatever unrelated code allocated next.
///
/// Clamping is the only response available on the audio thread: growing the
/// buffer means allocating there, which is worse than dropping the extra
/// frames of INPUT for one callback.
constexpr UInt32 clamped_input_frames(UInt32 requested, UInt32 capacity) {
    return requested < capacity ? requested : capacity;
}

class CoreAudioDevice : public AudioDevice {
public:
    CoreAudioDevice(AudioDeviceID device_id);
    ~CoreAudioDevice() override;

    bool open(const DeviceConfig& config) override;
    void close() override;
    bool start(AudioCallback callback) override;
    void stop() override;

    bool is_open() const override { return is_open_; }
    bool is_running() const override { return is_running_; }
    DeviceInfo info() const override;
    double sample_rate() const override { return config_.sample_rate; }
    int buffer_size() const override { return config_.buffer_size; }
    std::optional<AudioIoTiming> audio_io_timing() const;

    /// Read CoreAudio's device latency and safety-offset properties without
    /// opening or re-clocking the device. Public only on this backend-internal
    /// type so its platform smoke can prove the production property path.
    static std::optional<AudioIoTiming> query_audio_io_timing(
        AudioDeviceID device_id,
        std::uint64_t route_instance_token,
        std::uint64_t calibration_generation);

    /// Returns the device's I/O thread workgroup (`os_workgroup_t`)
    /// queried via `kAudioDevicePropertyIOThreadOSWorkgroup` on
    /// macOS 13+, or `nullptr` on older targets / when the device
    /// does not publish a workgroup. The CoreAudio property query returns a
    /// retained object; this device owns that reference and releases it after
    /// callbacks and auxiliary clients have left, on replacement or close.
    void* callback_workgroup() const override;

    std::uint64_t xrun_count() const override {
        return xrun_counter_.load(std::memory_order_relaxed);
    }

    void reset_xrun_counter() override {
        xrun_counter_.store(0, std::memory_order_relaxed);
    }

    void set_workgroup_change_callback(WorkgroupChangeCallback callback) override;
    void quiesce_workgroup_changes() override;

public:
    /// True when input was requested but the resolved device has no input
    /// streams, so capture is digital silence rather than a quiet signal. A
    /// host can surface "no input device" instead of rendering an empty
    /// analyzer that looks like a broken UI. Valid after open().
    bool capture_expected_silent() const noexcept { return capture_expected_silent_; }

private:
    /// Duplex open: the device resolved from the system default OUTPUT may have
    /// no input streams. Re-point at an input-capable device when one can serve
    /// BOTH directions; returns false (leaving the output device bound, so
    /// playback still works) when no single device can, after warning.
    bool resolve_duplex_input_device();

    /// One line naming the device, the consequence, and the remedy.
    static void warn_capture_will_be_silent(const std::string& device_name);

    static OSStatus render_callback(
        void* inRefCon,
        AudioUnitRenderActionFlags* ioActionFlags,
        const AudioTimeStamp* inTimeStamp,
        UInt32 inBusNumber,
        UInt32 inNumberFrames,
        AudioBufferList* ioData);

    static OSStatus overload_listener(
        AudioObjectID inObjectID,
        UInt32 inNumberAddresses,
        const AudioObjectPropertyAddress* inAddresses,
        void* inClientData);

    // Fires (on a CoreAudio thread) when the SYSTEM default output device changes.
    // For a follow_default_ unit it live-switches the AudioUnit to the new default
    // so audio moves to newly-selected outputs (AirPods/headphones) without a
    // relaunch. Serialized against stop()/close() by switch_mutex_.
    static OSStatus default_output_changed_listener(
        AudioObjectID inObjectID,
        UInt32 inNumberAddresses,
        const AudioObjectPropertyAddress* inAddresses,
        void* inClientData);
    void switch_to_default_output();

    /// Query the active device for its IO-thread workgroup; adopt the caller's
    /// retained result into `workgroup_reference_`. No-op on older OS / when the device does
    /// not publish one.
    void query_callback_workgroup();
    void mark_audio_io_timing_stale() noexcept;
    void refresh_audio_io_timing_locked() const;
    void install_audio_io_timing_listeners_locked();
    void remove_audio_io_timing_listeners_locked();
    void retire_audio_io_timing_listener_context();

    static OSStatus audio_io_timing_changed_listener(
        AudioObjectID inObjectID,
        UInt32 inNumberAddresses,
        const AudioObjectPropertyAddress* inAddresses,
        void* inClientData);

    AudioDeviceID device_id_;
    AudioComponentInstance audio_unit_ = nullptr;
    DeviceConfig config_;
    AudioCallback callback_;
    bool is_open_ = false;
    bool is_running_ = false;
    bool input_enabled_ = false;
    bool capture_expected_silent_ = false;
    // False for an input-only unit (output IO disabled, bus 0 render callback
    // never fires); the render callback then hands the caller an empty output.
    bool output_enabled_ = true;
    uint64_t sample_position_ = 0;

#if defined(__APPLE__)
    CoreAudioWorkgroupReference workgroup_reference_;
#endif

    // The CoreAudio I/O thread already belongs to the device workgroup exposed
    // by kAudioDevicePropertyIOThreadOSWorkgroup; only auxiliary threads join
    // it. A device without a workgroup uses the Mach RT-priority fallback once
    // per render-thread lifetime.
    std::atomic<bool> fallback_priority_configured_{false};
    std::atomic<std::uint64_t> xrun_counter_{0};
    bool overload_listener_installed_ = false;

    // Default-output following: when the unit was opened against the system
    // default (no pinned device, output-only), track default-device changes and
    // re-point the unit live. switch_mutex_ serializes the listener's stop/start
    // against the main thread's stop()/close() so the unit can't be disposed while
    // the listener is mid-switch.
    bool follow_default_ = false;
    bool default_output_listener_installed_ = false;
    mutable std::mutex switch_mutex_;
    WorkgroupChangeCallback workgroup_change_callback_;
    bool workgroup_changes_quiesced_ = false;
    static constexpr std::size_t kTimingPropertyCount = 6;
    std::array<AudioObjectPropertyAddress, kTimingPropertyCount>
        timing_property_addresses_{};
    std::array<bool, kTimingPropertyCount> timing_listener_installed_{};
    std::unique_ptr<CoreAudioTimingListenerContext> timing_listener_context_;
    mutable std::optional<AudioIoTiming> audio_io_timing_;
    const std::uint64_t route_instance_token_;
    mutable std::atomic<std::uint64_t> calibration_generation_{0};
    mutable std::atomic<bool> calibration_generation_exhausted_{false};
    mutable std::atomic<bool> audio_io_timing_dirty_{true};

    // Buffers for the callback
    std::vector<float*> output_ptrs_;
    std::vector<float*> input_ptrs_;

    // Pre-allocated input capture buffers (avoids allocation in audio callback)
    std::vector<float> input_buffer_storage_;
    std::vector<AudioBuffer> input_audio_buffers_;
    AudioBufferList* input_buffer_list_ = nullptr;
    size_t input_buffer_list_size_ = 0;
    /// Frames the input storage was ALLOCATED for.
    ///
    /// The render callback is handed `inNumberFrames` by CoreAudio and used to
    /// declare that many frames of space to AudioUnitRender without checking
    /// this. A device that asks for more than was allocated then had
    /// AudioUnitRender memcpy past the end of the heap buffer — caught by
    /// AddressSanitizer as a 1880-byte heap-buffer-overflow on the audio
    /// thread, which corrupted the heap and aborted the process later, in
    /// whatever unrelated code allocated next.
    UInt32 input_buffer_frames_ = 0;
};

class CoreAudioSystem : public AudioSystem {
public:
    CoreAudioSystem();
    ~CoreAudioSystem() override;

    std::vector<DeviceInfo> enumerate_devices() override;
    std::unique_ptr<AudioDevice> create_device(const std::string& device_id) override;
    DeviceInfo default_output_device() override;
    DeviceInfo default_input_device() override;
    void set_device_change_callback(DeviceChangeCallback cb) override;

    /// Subscribe to default-device-change events. The callback fires
    /// on a CoreAudio thread; subscribers must marshal to the UI
    /// thread themselves if needed. Pass `nullptr` to clear.
    using DefaultDeviceChangeCallback = std::function<void(bool is_input)>;
    void set_default_device_change_callback(DefaultDeviceChangeCallback cb);

    static DeviceInfo query_device_info(AudioDeviceID device_id);
    static AudioDeviceID get_default_device(bool input);

private:
    static OSStatus device_list_changed(AudioObjectID, UInt32,
                                        const AudioObjectPropertyAddress*, void*);
    static OSStatus default_device_changed(AudioObjectID, UInt32,
                                           const AudioObjectPropertyAddress*, void*);
    DeviceChangeCallback device_change_cb_;
    DefaultDeviceChangeCallback default_device_change_cb_;
    bool listener_installed_ = false;
    bool default_listener_installed_ = false;
};

} // namespace pulp::audio::mac

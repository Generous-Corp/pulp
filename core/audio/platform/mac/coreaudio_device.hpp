#pragma once

#include <pulp/audio/device.hpp>
#include <pulp/audio/workgroup.hpp>
#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <mutex>
#include <cstdint>

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

private:
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

    AudioDeviceID device_id_;
    AudioComponentInstance audio_unit_ = nullptr;
    DeviceConfig config_;
    AudioCallback callback_;
    bool is_open_ = false;
    bool is_running_ = false;
    bool input_enabled_ = false;
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

    // Buffers for the callback
    std::vector<float*> output_ptrs_;
    std::vector<float*> input_ptrs_;

    // Pre-allocated input capture buffers (avoids allocation in audio callback)
    std::vector<float> input_buffer_storage_;
    std::vector<AudioBuffer> input_audio_buffers_;
    AudioBufferList* input_buffer_list_ = nullptr;
    size_t input_buffer_list_size_ = 0;
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

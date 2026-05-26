#pragma once

#include <pulp/audio/device.hpp>
#include <pulp/audio/workgroup.hpp>
#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cstdint>

#if defined(__APPLE__)
#include <os/workgroup.h>
#endif

namespace pulp::audio::mac {

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
    /// does not publish a workgroup. Owned by the device; valid for
    /// the lifetime of the open device.
    void* callback_workgroup() const override {
#if defined(__APPLE__)
        return reinterpret_cast<void*>(workgroup_);
#else
        return nullptr;
#endif
    }

    std::uint64_t xrun_count() const override {
        return xrun_counter_.load(std::memory_order_relaxed);
    }

    void reset_xrun_counter() override {
        xrun_counter_.store(0, std::memory_order_relaxed);
    }

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

    /// Query the active device for its IO-thread workgroup; cache it
    /// into `workgroup_`. No-op on older OS / when the device does
    /// not publish one.
    void query_callback_workgroup();

    AudioDeviceID device_id_;
    AudioComponentInstance audio_unit_ = nullptr;
    DeviceConfig config_;
    AudioCallback callback_;
    bool is_open_ = false;
    bool is_running_ = false;
    bool input_enabled_ = false;
    uint64_t sample_position_ = 0;

#if defined(__APPLE__)
    os_workgroup_t workgroup_ = nullptr;
#endif

    // RAII workgroup join that lives on the render thread. First
    // invocation of render_callback joins; leaves on device close().
    AudioWorkgroup wg_join_;
    std::atomic<bool> wg_joined_{false};
    std::atomic<std::uint64_t> xrun_counter_{0};
    bool overload_listener_installed_ = false;

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

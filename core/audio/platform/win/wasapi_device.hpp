#pragma once

#include <pulp/audio/device.hpp>

#ifndef _WIN32
#error "wasapi_device.hpp is Windows-only"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <thread>
#include <string>
#include <vector>

namespace pulp::audio::win {

// WASAPI shared-mode audio device with event-driven rendering
class WasapiDevice : public AudioDevice {
public:
    explicit WasapiDevice(IMMDevice* device);  // Takes ownership (AddRef'd by caller)
    ~WasapiDevice() override;

    bool open(const DeviceConfig& config) override;
    void close() override;
    bool start(AudioCallback callback) override;
    void stop() override;

    bool is_open() const override { return is_open_; }
    bool is_running() const override { return is_running_.load(std::memory_order_relaxed); }
    DeviceInfo info() const override;
    double sample_rate() const override { return config_.sample_rate; }
    int buffer_size() const override { return config_.buffer_size; }

    // Shared helpers used by WasapiSystem during device enumeration.
    static std::wstring get_device_name(IMMDevice* device);
    static std::string wide_to_utf8(const std::wstring& wide);

private:
    void render_thread_func();

    IMMDevice* device_ = nullptr;
    IAudioClient* audio_client_ = nullptr;
    IAudioRenderClient* render_client_ = nullptr;
    HANDLE buffer_event_ = nullptr;
    HANDLE stop_event_ = nullptr;

    DeviceConfig config_;
    AudioCallback callback_;
    bool is_open_ = false;
    std::atomic<bool> is_running_{false};
    uint64_t sample_position_ = 0;
    UINT32 buffer_frames_ = 0;
    int actual_channels_ = 0;

    std::thread render_thread_;

    // Deinterleave buffer for callback
    std::vector<std::vector<float>> channel_buffers_;
    std::vector<float*> output_ptrs_;
};

class WasapiNotificationClient;  // fwd, issue #243 hotplug

class WasapiSystem : public AudioSystem {
public:
    WasapiSystem();
    ~WasapiSystem();

    std::vector<DeviceInfo> enumerate_devices() override;
    std::unique_ptr<AudioDevice> create_device(const std::string& device_id) override;
    DeviceInfo default_output_device() override;
    DeviceInfo default_input_device() override;

    static DeviceInfo query_device_info(IMMDevice* device);

private:
    IMMDeviceEnumerator* enumerator_ = nullptr;
    // Workstream 02 #243: IMMNotificationClient receives OS events when
    // audio endpoints are added/removed/activated/deactivated. We own
    // one instance per WasapiSystem and forward OnDeviceAdded etc. to
    // AudioSystem::fire_device_change so UI code can refresh its device
    // list without polling.
    WasapiNotificationClient* notifier_ = nullptr;
    bool com_initialized_ = false;
};

} // namespace pulp::audio::win

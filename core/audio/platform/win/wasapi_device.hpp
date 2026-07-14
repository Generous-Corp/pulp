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

// WASAPI shared-mode audio device with event-driven I/O.
//
// A single instance wraps EITHER a render endpoint OR a capture
// endpoint; the EDataFlow passed at construction selects which path
// open() / start() take. Duplex callers open two devices and
// synchronize externally — same model as the rest of the AudioSystem
// interface, which is direction-agnostic at the device level.
//
// ── WASAPI mode coverage ─────────────────────────────────────────────────
//
// `IAudioClient::Initialize` is invoked with `AUDCLNT_SHAREMODE_SHARED`
// + `AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST`
// at the device's native mix format. This is the default mode the
// Windows audio engine mixes; buffer size is requested in REFERENCE_TIME
// units and rounded up to the engine's period.
//
// `AUDCLNT_SHAREMODE_EXCLUSIVE` is now also supported (W4), selected via
// `DeviceConfig::share_mode == ShareMode::exclusive`: the endpoint is taken
// exclusively at the device mix format (so the render/capture threads' existing
// planar↔interleaved conversion still applies), driven at the device's minimum
// period for low latency, with the documented AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED
// re-activation retry. Shared mode stays the default.
//
// `AUDCLNT_SHAREMODE_SHARED` low-latency (W4b) is supported via IAudioClient3:
// when `DeviceConfig::low_latency` is set on a shared-mode open we query
// `GetSharedModeEnginePeriod` and call `InitializeSharedAudioStream` at the
// engine's MINIMUM period (so the engine glitch-free buffer is as small as the
// driver allows). If IAudioClient3 cannot be obtained, the format is rejected
// by `IsFormatSupported`, the min period is out of the supported range, or
// `InitializeSharedAudioStream` fails for any reason, we honestly degrade to
// the standard shared-mode `Initialize` at the requested buffer size — never a
// half-open stream.
//
// Device-invalidation / sample-rate-change recovery (W4b): the render/capture
// threads watch for `AUDCLNT_E_DEVICE_INVALIDATED` (the HRESULT WASAPI returns
// from GetBuffer / GetCurrentPadding / GetNextPacketSize when the endpoint
// disappears, is reconfigured, or its mix format / sample rate changes under
// us). On that error the thread stops cleanly (sets is_running_ false, breaks
// its loop) and fires the AudioSystem device-change notification so the host
// can re-open the device at the new format. Transparent in-place reopen is
// deliberately NOT attempted — clean-stop + notify is the contract, matching
// the hotplug notifier.
class WasapiDevice : public AudioDevice {
public:
    explicit WasapiDevice(IMMDevice* device, EDataFlow flow = eRender);
    ~WasapiDevice() override;

    // Set the owning AudioSystem so the I/O thread can fire device-change
    // notifications on AUDCLNT_E_DEVICE_INVALIDATED (W4b). WasapiSystem wires
    // this in create_device(). May be nullptr; if so, invalidation still stops
    // the stream cleanly but no notification is dispatched.
    void set_owner(AudioSystem* owner) { owner_ = owner; }

    bool open(const DeviceConfig& config) override;
    void close() override;
    bool start(AudioCallback callback) override;
    void stop() override;

    bool is_open() const override { return is_open_; }
    bool is_running() const override { return is_running_.load(std::memory_order_relaxed); }
    DeviceInfo info() const override;
    double sample_rate() const override { return config_.sample_rate; }
    int buffer_size() const override { return config_.buffer_size; }

    /// Direction this device wraps. eRender = output, eCapture = input.
    EDataFlow flow() const { return flow_; }

    // Shared helpers used by WasapiSystem during device enumeration.
    static std::wstring get_device_name(IMMDevice* device);
    static std::string wide_to_utf8(const std::wstring& wide);

private:
    void render_thread_func();
    void capture_thread_func();

    // Exclusive-mode Initialize on audio_client_ at `fmt`, driven at the device
    // minimum period. Handles the AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED dance
    // (re-Activate audio_client_ with the aligned period). Returns the final
    // HRESULT; on success audio_client_ is initialized and event-driven.
    HRESULT initialize_exclusive_(WAVEFORMATEX* fmt);

    // Shared-mode low-latency Initialize via IAudioClient3 (W4b). Queries
    // GetSharedModeEnginePeriod and calls InitializeSharedAudioStream at the
    // engine's minimum supported period. Returns the HRESULT from
    // InitializeSharedAudioStream on success, or a failure HRESULT if
    // IAudioClient3 is unavailable / the format or period is unsupported — in
    // which case the caller falls back to the standard shared Initialize.
    HRESULT initialize_shared_low_latency_(WAVEFORMATEX* fmt);

    // Handle AUDCLNT_E_DEVICE_INVALIDATED from an I/O thread (W4b): mark the
    // stream not-running and fire the owning AudioSystem's device-change
    // notification so the host can re-open. Idempotent-safe to call once per
    // invalidation; the caller breaks its loop afterwards.
    void on_device_invalidated_();

    IMMDevice*           device_         = nullptr;
    EDataFlow            flow_           = eRender;
    AudioSystem*         owner_          = nullptr;  // for device-change notify (W4b)
    IAudioClient*        audio_client_   = nullptr;
    IAudioRenderClient*  render_client_  = nullptr;  // populated when flow_ == eRender
    IAudioCaptureClient* capture_client_ = nullptr;  // populated when flow_ == eCapture
    HANDLE buffer_event_ = nullptr;
    HANDLE stop_event_ = nullptr;

    DeviceConfig config_;
    AudioCallback callback_;
    bool is_open_ = false;
    std::atomic<bool> is_running_{false};
    uint64_t sample_position_ = 0;
    UINT32 buffer_frames_ = 0;
    int actual_channels_ = 0;     ///< channels delivered to the user callback
    int engine_channels_ = 0;     ///< channels in the WASAPI packet stride

    std::thread io_thread_;

    // Deinterleave / planar buffers for the callback.  For render
    // direction these are output channels; for capture they're the
    // input channels we hand to the user via BufferView<const float>.
    std::vector<std::vector<float>> channel_buffers_;
    std::vector<float*> channel_ptrs_;
};

class WasapiNotificationClient;  // hotplug notifier

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
    // IMMNotificationClient receives OS events when audio endpoints are
    // added/removed/activated/deactivated. We own one instance per
    // WasapiSystem and forward OnDeviceAdded etc. to
    // AudioSystem::fire_device_change so UI code can refresh its device list
    // without polling.
    WasapiNotificationClient* notifier_ = nullptr;
    bool com_initialized_ = false;
};

} // namespace pulp::audio::win

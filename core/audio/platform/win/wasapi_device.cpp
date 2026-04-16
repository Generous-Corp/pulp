#include "wasapi_device.hpp"
#include <pulp/runtime/log.hpp>

#include <combaseapi.h>
#include <atomic>
#include <cstring>
#include <algorithm>

// REFERENCE_TIME units: 100-nanosecond intervals
static constexpr long long REFTIMES_PER_SEC = 10'000'000LL;

namespace pulp::audio::win {

// ── WasapiDevice ─────────────────────────────────────────────────────────

WasapiDevice::WasapiDevice(IMMDevice* device)
    : device_(device)
{
    // Caller has already AddRef'd
}

WasapiDevice::~WasapiDevice() {
    if (is_running_.load()) stop();
    if (is_open_) close();
    if (device_) { device_->Release(); device_ = nullptr; }
}

bool WasapiDevice::open(const DeviceConfig& config) {
    config_ = config;

    // Activate the audio client
    HRESULT hr = device_->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL,
        nullptr, reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr)) {
        runtime::log_error("WASAPI: could not activate audio client (0x{:08x})", static_cast<unsigned>(hr));
        return false;
    }

    // Get the mix format (what the device natively supports in shared mode)
    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client_->GetMixFormat(&mix_format);
    if (FAILED(hr)) {
        runtime::log_error("WASAPI: could not get mix format (0x{:08x})", static_cast<unsigned>(hr));
        close();
        return false;
    }

    // Use the device's native format in shared mode
    // Record the actual channel count and sample rate
    actual_channels_ = std::min(static_cast<int>(mix_format->nChannels), config_.output_channels);
    config_.sample_rate = static_cast<double>(mix_format->nSamplesPerSec);

    // Calculate buffer duration from requested buffer size
    REFERENCE_TIME requested_duration =
        static_cast<REFERENCE_TIME>(
            static_cast<double>(config_.buffer_size) / config_.sample_rate * REFTIMES_PER_SEC);

    // Create events for buffer notification and stop signaling
    buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);  // Manual reset
    if (!buffer_event_ || !stop_event_) {
        runtime::log_error("WASAPI: could not create events");
        CoTaskMemFree(mix_format);
        close();
        return false;
    }

    // Initialize audio client in shared mode with event-driven buffering
    hr = audio_client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
        requested_duration,
        0,  // periodicity (must be 0 for shared mode)
        mix_format,
        nullptr);  // session GUID

    CoTaskMemFree(mix_format);

    if (FAILED(hr)) {
        runtime::log_error("WASAPI: could not initialize audio client (0x{:08x})", static_cast<unsigned>(hr));
        close();
        return false;
    }

    // Set the event handle for buffer notifications
    hr = audio_client_->SetEventHandle(buffer_event_);
    if (FAILED(hr)) {
        runtime::log_error("WASAPI: could not set event handle (0x{:08x})", static_cast<unsigned>(hr));
        close();
        return false;
    }

    // Get the actual buffer size allocated by WASAPI
    hr = audio_client_->GetBufferSize(&buffer_frames_);
    if (FAILED(hr)) {
        runtime::log_error("WASAPI: could not get buffer size (0x{:08x})", static_cast<unsigned>(hr));
        close();
        return false;
    }
    config_.buffer_size = static_cast<int>(buffer_frames_);

    // Get the render client interface
    hr = audio_client_->GetService(
        __uuidof(IAudioRenderClient),
        reinterpret_cast<void**>(&render_client_));
    if (FAILED(hr)) {
        runtime::log_error("WASAPI: could not get render client (0x{:08x})", static_cast<unsigned>(hr));
        close();
        return false;
    }

    // Pre-allocate deinterleave buffers
    channel_buffers_.resize(actual_channels_);
    output_ptrs_.resize(actual_channels_);
    for (int ch = 0; ch < actual_channels_; ++ch) {
        channel_buffers_[ch].resize(buffer_frames_, 0.0f);
        output_ptrs_[ch] = channel_buffers_[ch].data();
    }

    is_open_ = true;
    runtime::log_info("WASAPI: opened device '{}' at {} Hz, buffer {} frames, {} channels",
        info().name, config_.sample_rate, buffer_frames_, actual_channels_);
    return true;
}

void WasapiDevice::close() {
    if (render_client_) { render_client_->Release(); render_client_ = nullptr; }
    if (audio_client_) { audio_client_->Release(); audio_client_ = nullptr; }
    if (buffer_event_) { CloseHandle(buffer_event_); buffer_event_ = nullptr; }
    if (stop_event_) { CloseHandle(stop_event_); stop_event_ = nullptr; }
    channel_buffers_.clear();
    output_ptrs_.clear();
    is_open_ = false;
}

bool WasapiDevice::start(AudioCallback callback) {
    if (!is_open_) return false;
    callback_ = std::move(callback);
    sample_position_ = 0;

    // Reset stop event
    ResetEvent(stop_event_);

    // Pre-fill the buffer with silence before starting
    BYTE* data = nullptr;
    HRESULT hr = render_client_->GetBuffer(buffer_frames_, &data);
    if (SUCCEEDED(hr)) {
        std::memset(data, 0, buffer_frames_ * actual_channels_ * sizeof(float));
        render_client_->ReleaseBuffer(buffer_frames_, 0);
    }

    // Start the audio stream
    hr = audio_client_->Start();
    if (FAILED(hr)) {
        runtime::log_error("WASAPI: could not start audio client (0x{:08x})", static_cast<unsigned>(hr));
        return false;
    }

    is_running_.store(true, std::memory_order_release);

    // Launch the render thread
    render_thread_ = std::thread([this] { render_thread_func(); });

    return true;
}

void WasapiDevice::stop() {
    if (!is_running_.load(std::memory_order_acquire)) return;

    // Signal the render thread to stop
    is_running_.store(false, std::memory_order_release);
    if (stop_event_) SetEvent(stop_event_);

    // Wait for the render thread to finish
    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    if (audio_client_) {
        audio_client_->Stop();
        audio_client_->Reset();
    }

    callback_ = nullptr;
}

DeviceInfo WasapiDevice::info() const {
    return WasapiSystem::query_device_info(device_);
}

void WasapiDevice::render_thread_func() {
    // Set thread priority for real-time audio
    // AVRT would be ideal but requires linking avrt.lib — use high priority instead
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    HANDLE wait_handles[] = { buffer_event_, stop_event_ };

    while (is_running_.load(std::memory_order_relaxed)) {
        // Wait for buffer event or stop signal
        DWORD result = WaitForMultipleObjects(2, wait_handles, FALSE, 2000);

        if (result == WAIT_OBJECT_0 + 1) {
            // Stop event signaled
            break;
        }
        if (result == WAIT_TIMEOUT) {
            continue;
        }
        if (result != WAIT_OBJECT_0) {
            // Error
            runtime::log_error("WASAPI: wait failed ({})", result);
            break;
        }

        // How many frames are available in the buffer?
        UINT32 padding = 0;
        HRESULT hr = audio_client_->GetCurrentPadding(&padding);
        if (FAILED(hr)) continue;

        UINT32 available = buffer_frames_ - padding;
        if (available == 0) continue;

        // Get the output buffer from WASAPI
        BYTE* data = nullptr;
        hr = render_client_->GetBuffer(available, &data);
        if (FAILED(hr)) continue;

        auto* interleaved = reinterpret_cast<float*>(data);

        if (callback_) {
            // Resize channel buffers if needed
            for (int ch = 0; ch < actual_channels_; ++ch) {
                if (channel_buffers_[ch].size() < available) {
                    channel_buffers_[ch].resize(available);
                    output_ptrs_[ch] = channel_buffers_[ch].data();
                }
                // Clear the buffers
                std::memset(output_ptrs_[ch], 0, available * sizeof(float));
            }

            // Call the user callback with non-interleaved buffers
            BufferView<const float> input_view;  // No input for now
            BufferView<float> output_view(output_ptrs_.data(),
                static_cast<size_t>(actual_channels_), available);

            CallbackContext ctx;
            ctx.sample_rate = config_.sample_rate;
            ctx.buffer_size = static_cast<int>(available);
            ctx.sample_position = sample_position_;

            callback_(input_view, output_view, ctx);

            // Interleave the output into WASAPI's buffer
            for (UINT32 frame = 0; frame < available; ++frame) {
                for (int ch = 0; ch < actual_channels_; ++ch) {
                    interleaved[frame * actual_channels_ + ch] = output_ptrs_[ch][frame];
                }
            }
        } else {
            // Silence
            std::memset(data, 0, available * actual_channels_ * sizeof(float));
        }

        render_client_->ReleaseBuffer(available, 0);
        sample_position_ += available;
    }
}

std::wstring WasapiDevice::get_device_name(IMMDevice* device) {
    IPropertyStore* props = nullptr;
    HRESULT hr = device->OpenPropertyStore(STGM_READ, &props);
    if (FAILED(hr)) return L"Unknown";

    PROPVARIANT var;
    PropVariantInit(&var);
    hr = props->GetValue(PKEY_Device_FriendlyName, &var);

    std::wstring name;
    if (SUCCEEDED(hr) && var.vt == VT_LPWSTR) {
        name = var.pwszVal;
    } else {
        name = L"Unknown";
    }

    PropVariantClear(&var);
    props->Release();
    return name;
}

std::string WasapiDevice::wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
        static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
        static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
}

// ── WasapiSystem ─────────────────────────────────────────────────────────

// IMMNotificationClient — hotplug receiver for WASAPI (issue #243).
//
// Windows calls our OnDeviceAdded / OnDeviceRemoved / OnDeviceStateChanged /
// OnDefaultDeviceChanged on a background thread owned by mmdevapi. We
// forward them into AudioSystem::fire_device_change so UI code subscribed
// via on_device_change sees hotplug without polling.
class WasapiNotificationClient : public IMMNotificationClient {
public:
    explicit WasapiNotificationClient(WasapiSystem* owner) : owner_(owner) {}

    // IUnknown — COM refcounting is touched from mmdevapi's background
    // threads during hotplug, so the counter must be atomic. Plain
    // `++`/`--` raced with the default-device-change thread. (Codex P1
    // on the original PR.)
    ULONG STDMETHODCALLTYPE AddRef() override  {
        return static_cast<ULONG>(
            refcount_.fetch_add(1, std::memory_order_acq_rel) + 1);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG prev = static_cast<ULONG>(
            refcount_.fetch_sub(1, std::memory_order_acq_rel));
        if (prev == 1) delete this;
        return prev - 1;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient — all forward through fire_device_change.
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR)             override { if (owner_) owner_->fire_device_change(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR)           override { if (owner_) owner_->fire_device_change(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { if (owner_) owner_->fire_device_change(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { if (owner_) owner_->fire_device_change(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    WasapiSystem* owner_;
    std::atomic<LONG> refcount_{1};
};

WasapiSystem::WasapiSystem() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    com_initialized_ = SUCCEEDED(hr) || hr == S_FALSE;  // S_FALSE = already initialized

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) {
        runtime::log_error("WASAPI: could not create device enumerator (0x{:08x})", static_cast<unsigned>(hr));
        enumerator_ = nullptr;
        return;
    }

    // Register hotplug notifier (issue #243).
    notifier_ = new WasapiNotificationClient(this);
    if (FAILED(enumerator_->RegisterEndpointNotificationCallback(notifier_))) {
        runtime::log_warn("WASAPI: RegisterEndpointNotificationCallback failed; "
                          "device hotplug will not notify AudioSystem listeners");
        notifier_->Release();
        notifier_ = nullptr;
    }
}

WasapiSystem::~WasapiSystem() {
    if (enumerator_ && notifier_) {
        enumerator_->UnregisterEndpointNotificationCallback(notifier_);
    }
    if (notifier_) notifier_->Release();
    if (enumerator_) enumerator_->Release();
    if (com_initialized_) CoUninitialize();
}

DeviceInfo WasapiSystem::query_device_info(IMMDevice* device) {
    DeviceInfo info;

    // Get device ID
    LPWSTR device_id = nullptr;
    if (SUCCEEDED(device->GetId(&device_id))) {
        info.id = WasapiDevice::wide_to_utf8(device_id);
        CoTaskMemFree(device_id);
    }

    // Get friendly name
    info.name = WasapiDevice::wide_to_utf8(WasapiDevice::get_device_name(device));

    // Get channel count and sample rate from the mix format
    IAudioClient* client = nullptr;
    HRESULT hr = device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL,
        nullptr, reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr) && client) {
        WAVEFORMATEX* format = nullptr;
        hr = client->GetMixFormat(&format);
        if (SUCCEEDED(hr) && format) {
            info.max_output_channels = static_cast<int>(format->nChannels);
            info.sample_rates.push_back(static_cast<double>(format->nSamplesPerSec));
            CoTaskMemFree(format);
        }
        client->Release();
    }

    // Standard buffer sizes
    info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};

    return info;
}

std::vector<DeviceInfo> WasapiSystem::enumerate_devices() {
    std::vector<DeviceInfo> devices;
    if (!enumerator_) return devices;

    // Get the default output device ID for comparison
    IMMDevice* default_out = nullptr;
    std::string default_out_id;
    if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &default_out))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(default_out->GetId(&id))) {
            default_out_id = WasapiDevice::wide_to_utf8(id);
            CoTaskMemFree(id);
        }
        default_out->Release();
    }

    IMMDevice* default_in = nullptr;
    std::string default_in_id;
    if (SUCCEEDED(enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &default_in))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(default_in->GetId(&id))) {
            default_in_id = WasapiDevice::wide_to_utf8(id);
            CoTaskMemFree(id);
        }
        default_in->Release();
    }

    // Enumerate render (output) devices
    IMMDeviceCollection* collection = nullptr;
    HRESULT hr = enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(collection->Item(i, &device))) {
                auto info = query_device_info(device);
                info.is_default_output = (info.id == default_out_id);
                devices.push_back(std::move(info));
                device->Release();
            }
        }
        collection->Release();
    }

    // Enumerate capture (input) devices
    hr = enumerator_->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr) && collection) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (SUCCEEDED(collection->Item(i, &device))) {
                auto info = query_device_info(device);
                info.max_input_channels = info.max_output_channels;
                info.max_output_channels = 0;
                info.is_default_input = (info.id == default_in_id);
                devices.push_back(std::move(info));
                device->Release();
            }
        }
        collection->Release();
    }

    return devices;
}

std::unique_ptr<AudioDevice> WasapiSystem::create_device(const std::string& device_id) {
    if (!enumerator_) return nullptr;

    IMMDevice* device = nullptr;
    if (device_id.empty()) {
        // Use default output device
        HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            runtime::log_error("WASAPI: could not get default output device (0x{:08x})", static_cast<unsigned>(hr));
            return nullptr;
        }
    } else {
        // Convert UTF-8 ID to wide string
        int wide_len = MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(),
            static_cast<int>(device_id.size()), nullptr, 0);
        std::wstring wide_id(wide_len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, device_id.c_str(),
            static_cast<int>(device_id.size()), wide_id.data(), wide_len);

        HRESULT hr = enumerator_->GetDevice(wide_id.c_str(), &device);
        if (FAILED(hr)) {
            runtime::log_error("WASAPI: could not get device '{}' (0x{:08x})", device_id, static_cast<unsigned>(hr));
            return nullptr;
        }
    }

    return std::make_unique<WasapiDevice>(device);
}

DeviceInfo WasapiSystem::default_output_device() {
    if (!enumerator_) return {};
    IMMDevice* device = nullptr;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return {};
    auto info = query_device_info(device);
    info.is_default_output = true;
    device->Release();
    return info;
}

DeviceInfo WasapiSystem::default_input_device() {
    if (!enumerator_) return {};
    IMMDevice* device = nullptr;
    if (FAILED(enumerator_->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) return {};
    auto info = query_device_info(device);
    info.is_default_input = true;
    info.max_input_channels = info.max_output_channels;
    info.max_output_channels = 0;
    device->Release();
    return info;
}

} // namespace pulp::audio::win

// Factory function
namespace pulp::audio {

std::unique_ptr<AudioSystem> create_audio_system() {
    return std::make_unique<win::WasapiSystem>();
}

} // namespace pulp::audio

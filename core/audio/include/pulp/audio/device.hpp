#pragma once

#include <pulp/audio/buffer.hpp>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include <cstdint>
#include <utility>

namespace pulp::audio {

// Information about an audio device
struct DeviceInfo {
    std::string id;              // Platform-specific unique ID
    std::string name;            // Human-readable name
    int max_input_channels = 0;
    int max_output_channels = 0;
    std::vector<double> sample_rates;  // Supported sample rates
    std::vector<int> buffer_sizes;     // Supported buffer sizes
    bool is_default_input = false;
    bool is_default_output = false;
};

// Configuration for opening a device
struct DeviceConfig {
    std::string device_id;       // Empty = default device
    double sample_rate = 48000.0;
    int buffer_size = 256;
    int input_channels = 0;
    int output_channels = 2;
};

// Audio callback context — passed to the render function
struct CallbackContext {
    double sample_rate = 0;
    int buffer_size = 0;
    uint64_t sample_position = 0;  // Running sample count
};

// The audio callback: receives input, fills output
// Called on the real-time audio thread — no allocation, no locks, no blocking.
using AudioCallback = std::function<void(
    const BufferView<const float>& input,
    BufferView<float>& output,
    const CallbackContext& context
)>;

// Abstract audio device interface
class AudioDevice {
public:
    virtual ~AudioDevice() = default;

    virtual bool open(const DeviceConfig& config) = 0;
    virtual void close() = 0;
    virtual bool start(AudioCallback callback) = 0;
    virtual void stop() = 0;

    virtual bool is_open() const = 0;
    virtual bool is_running() const = 0;
    virtual DeviceInfo info() const = 0;
    virtual double sample_rate() const = 0;
    virtual int buffer_size() const = 0;

    /// Opaque handle to the OS-supplied real-time workgroup associated
    /// with this device's I/O thread, or `nullptr` if the platform does
    /// not expose one. On macOS 13+ / iOS 16+ this returns the
    /// `os_workgroup_t` queried via `kAudioDevicePropertyIOThreadOSWorkgroup`;
    /// on older Apple targets and on non-Apple platforms this returns
    /// `nullptr` and callers fall back to
    /// `AudioWorkgroup::set_realtime_priority()`.
    ///
    /// The returned pointer is owned by the device and remains valid
    /// for the lifetime of the open device. Callers must not free it.
    /// Pass it to `AudioWorkgroup::set_workgroup(static_cast<os_workgroup_t>(...))`
    /// before joining from the audio thread.
    ///
    /// Default returns `nullptr` so non-Apple backends inherit the
    /// best-effort priority fallback for free.
    virtual void* callback_workgroup() const { return nullptr; }

    /// Cumulative xrun (overload/underrun) count since `start()` or
    /// the last `reset_xrun_counter()`. CoreAudio devices increment on
    /// every `kAudioDeviceProcessorOverload` notification; backends
    /// that do not surface overload events stay at 0.
    virtual std::uint64_t xrun_count() const { return 0; }

    /// Reset the xrun counter to 0. Safe from any thread.
    virtual void reset_xrun_counter() {}
};

// Audio system — enumerates devices and creates device instances
class AudioSystem {
public:
    virtual ~AudioSystem() = default;

    virtual std::vector<DeviceInfo> enumerate_devices() = 0;
    virtual std::unique_ptr<AudioDevice> create_device(const std::string& device_id = "") = 0;
    virtual DeviceInfo default_output_device() = 0;
    virtual DeviceInfo default_input_device() = 0;

    /// Register a callback for device list changes (hotplug/unplug).
    /// The callback may fire on an OS thread — callers should dispatch to the
    /// main/UI thread if needed. Pass nullptr to unregister.
    using DeviceChangeCallback = std::function<void()>;

    /// Default implementation stores the callback in a base-class slot.
    /// Non-macOS backends that gain real hotplug probing in later slices
    /// (workstream 02 — WASAPI IMMNotificationClient, ALSA udev monitor,
    /// Win32 MIDI DeviceWatcher, ALSA seq monitor) call `fire_device_change()`
    /// when a change is detected. Backends with richer semantics can still
    /// override this entirely.
    virtual void set_device_change_callback(DeviceChangeCallback cb) {
        std::lock_guard<std::mutex> lock(device_change_callback_mutex_);
        if (cb) {
            device_change_callback_ = std::make_shared<DeviceChangeCallback>(std::move(cb));
        } else {
            device_change_callback_.reset();
        }
    }

    /// Dispatch a stored device-change callback. Safe to call from any
    /// thread; the callback itself is responsible for UI-thread
    /// marshalling.
    /// The callback is retained before invocation so it may unregister or
    /// replace itself during dispatch without invalidating the in-flight call.
    ///
    /// **Must be public, not protected.** Platform notifiers
    /// (WasapiNotificationClient, Win32 MIDI DeviceWatcher, ALSA
    /// seq monitor, etc.) are NOT AudioSystem subclasses — they own
    /// a pointer to an AudioSystem and invoke this method from an
    /// OS callback thread. C++ `protected` would require the access
    /// to go through the notifier's own derived-class `this`, which
    /// doesn't apply here. MSVC enforces this strictly and failed
    /// to build #281's WASAPI hotplug path (C2248 in
    /// wasapi_device.cpp:339–341), silently breaking release-cli.yml
    /// on Windows x64 for v0.15.0 and v0.16.0 tags. Clang/GCC were
    /// lenient; making this public matches the design intent the
    /// doc comment always described.
    void fire_device_change() {
        std::shared_ptr<DeviceChangeCallback> callback;
        {
            std::lock_guard<std::mutex> lock(device_change_callback_mutex_);
            callback = device_change_callback_;
        }
        if (callback && *callback) (*callback)();
    }
    /// Observe whether a caller registered a callback.
    bool has_device_change_callback() const {
        std::lock_guard<std::mutex> lock(device_change_callback_mutex_);
        return static_cast<bool>(device_change_callback_ && *device_change_callback_);
    }

private:
    mutable std::mutex device_change_callback_mutex_;
    std::shared_ptr<DeviceChangeCallback> device_change_callback_;
};

// Create the platform-appropriate audio system
std::unique_ptr<AudioSystem> create_audio_system();

} // namespace pulp::audio

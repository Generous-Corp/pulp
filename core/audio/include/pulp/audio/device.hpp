#pragma once

#include <pulp/audio/buffer.hpp>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>

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
        device_change_callback_ = std::move(cb);
    }

protected:
    /// Dispatch a stored device-change callback. Safe to call from any
    /// thread; the callback itself is responsible for UI-thread marshalling.
    void fire_device_change() {
        if (device_change_callback_) device_change_callback_();
    }
    /// Accessible to subclasses that want to observe whether a caller
    /// registered a callback.
    bool has_device_change_callback() const {
        return static_cast<bool>(device_change_callback_);
    }

private:
    DeviceChangeCallback device_change_callback_;
};

// Create the platform-appropriate audio system
std::unique_ptr<AudioSystem> create_audio_system();

} // namespace pulp::audio

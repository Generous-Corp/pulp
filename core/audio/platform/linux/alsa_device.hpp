#pragma once

#include <pulp/audio/device.hpp>

#ifndef __linux__
#error "alsa_device.hpp is Linux-only"
#endif

#include <alsa/asoundlib.h>

#include <pulp/runtime/udev_monitor.hpp>

#include <atomic>
#include <thread>
#include <string>
#include <vector>

namespace pulp::audio::linux_platform {

// ALSA audio device. Single instance wraps EITHER a PLAYBACK
// (output) endpoint OR a CAPTURE (input) endpoint, selected via the
// stream parameter at construction. Duplex callers open two devices
// and synchronize externally — same model as WASAPI on Windows and
// CoreAudio on macOS.
class AlsaDevice : public AudioDevice {
public:
    explicit AlsaDevice(const std::string& device_name,
                        snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK);
    ~AlsaDevice() override;

    bool open(const DeviceConfig& config) override;
    void close() override;
    bool start(AudioCallback callback) override;
    void stop() override;

    bool is_open() const override { return is_open_; }
    bool is_running() const override { return is_running_.load(std::memory_order_relaxed); }
    DeviceInfo info() const override;
    double sample_rate() const override { return config_.sample_rate; }
    int buffer_size() const override { return config_.buffer_size; }

    /// Direction this device wraps (PLAYBACK = output, CAPTURE = input).
    snd_pcm_stream_t stream() const { return stream_; }

private:
    void render_thread_func();
    void capture_thread_func();

    std::string device_name_;
    snd_pcm_stream_t stream_ = SND_PCM_STREAM_PLAYBACK;
    snd_pcm_t* pcm_ = nullptr;
    DeviceConfig config_;
    AudioCallback callback_;
    bool is_open_ = false;
    std::atomic<bool> is_running_{false};
    uint64_t sample_position_ = 0;
    snd_pcm_uframes_t period_size_ = 0;
    int actual_channels_ = 0;

    std::thread io_thread_;

    // Non-interleaved buffers for callback. For PLAYBACK these are
    // user-filled output channels; for CAPTURE they're the input we
    // hand the user as BufferView<const float>.
    std::vector<std::vector<float>> channel_buffers_;
    std::vector<float*> channel_ptrs_;
    // Interleaved buffer for ALSA read or write.
    std::vector<float> interleaved_;
};

class AlsaSystem : public AudioSystem {
public:
    ~AlsaSystem() override;  // stops the hotplug monitor before the base dtor

    std::vector<DeviceInfo> enumerate_devices() override;
    std::unique_ptr<AudioDevice> create_device(const std::string& device_id) override;
    DeviceInfo default_output_device() override;
    DeviceInfo default_input_device() override;

    /// Start (or stop) a libudev "sound"-subsystem monitor that calls
    /// `fire_device_change()` on card add/remove. Honest no-op for hotplug if
    /// libudev is unavailable — the callback is still stored, just never fired.
    void set_device_change_callback(DeviceChangeCallback cb) override;

private:
    runtime::UdevMonitor hotplug_monitor_;
};

} // namespace pulp::audio::linux_platform

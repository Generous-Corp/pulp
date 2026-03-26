#pragma once

#include <pulp/audio/device.hpp>

#ifndef __linux__
#error "alsa_device.hpp is Linux-only"
#endif

#include <alsa/asoundlib.h>

#include <atomic>
#include <thread>
#include <string>
#include <vector>

namespace pulp::audio::linux_platform {

// ALSA audio device with double-buffered write-ahead rendering
class AlsaDevice : public AudioDevice {
public:
    explicit AlsaDevice(const std::string& device_name);
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

private:
    void render_thread_func();

    std::string device_name_;
    snd_pcm_t* pcm_ = nullptr;
    DeviceConfig config_;
    AudioCallback callback_;
    bool is_open_ = false;
    std::atomic<bool> is_running_{false};
    uint64_t sample_position_ = 0;
    snd_pcm_uframes_t period_size_ = 0;
    int actual_channels_ = 0;

    std::thread render_thread_;

    // Non-interleaved buffers for callback
    std::vector<std::vector<float>> channel_buffers_;
    std::vector<float*> output_ptrs_;
    // Interleaved buffer for ALSA write
    std::vector<float> interleaved_;
};

class AlsaSystem : public AudioSystem {
public:
    std::vector<DeviceInfo> enumerate_devices() override;
    std::unique_ptr<AudioDevice> create_device(const std::string& device_id) override;
    DeviceInfo default_output_device() override;
    DeviceInfo default_input_device() override;
};

} // namespace pulp::audio::linux_platform

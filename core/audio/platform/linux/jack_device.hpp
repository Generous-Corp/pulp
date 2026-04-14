#pragma once

#include <pulp/audio/device.hpp>

#ifndef __linux__
#error "jack_device.hpp is Linux-only"
#endif

#include <jack/jack.h>

#include <atomic>
#include <string>
#include <vector>

namespace pulp::audio::linux_platform {

// JACK audio client — zero-copy, lowest-latency Linux audio
// Connects to the JACK server (or PipeWire's JACK compatibility layer).
// The JACK server calls our process callback directly on the audio thread.
class JackDevice : public AudioDevice {
public:
    explicit JackDevice(const std::string& client_name);
    ~JackDevice() override;

    bool open(const DeviceConfig& config) override;
    void close() override;
    bool start(AudioCallback callback) override;
    void stop() override;

    bool is_open() const override { return client_ != nullptr; }
    bool is_running() const override { return is_running_.load(std::memory_order_relaxed); }
    DeviceInfo info() const override;
    double sample_rate() const override;
    int buffer_size() const override;

private:
    static int process_callback(jack_nframes_t nframes, void* arg);
    static void shutdown_callback(void* arg);

    std::string client_name_;
    jack_client_t* client_ = nullptr;
    std::vector<jack_port_t*> output_ports_;
    std::vector<jack_port_t*> input_ports_;
    DeviceConfig config_;
    AudioCallback callback_;
    std::atomic<bool> is_running_{false};
    uint64_t sample_position_ = 0;
};

// Check if a JACK server is available
bool jack_is_available();

/// AudioSystem wrapper that routes create_device() to JackDevice.
/// Workstream 02 slice 2.2 — previously no JackSystem existed, so the
/// Linux factory unconditionally returned AlsaSystem even when a JACK
/// server was running.
class JackSystem : public AudioSystem {
public:
    std::vector<DeviceInfo> enumerate_devices() override;
    std::unique_ptr<AudioDevice> create_device(const std::string& device_id) override;
    DeviceInfo default_output_device() override;
    DeviceInfo default_input_device() override;
};

} // namespace pulp::audio::linux_platform

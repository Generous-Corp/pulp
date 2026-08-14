#pragma once

#include <pulp/audio/device.hpp>

#ifndef __linux__
#error "jack_device.hpp is Linux-only"
#endif

#include <jack/jack.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulp::audio::linux_platform {

namespace detail {

std::uint64_t next_linux_audio_route_instance_token() noexcept;

} // namespace detail

struct JackTimingValues {
    std::optional<std::uint32_t> input_total_latency_frames;
    std::optional<std::uint32_t> output_total_latency_frames;
    std::uint32_t period_frames = 0;
    double sample_rate_hz = 0.0;
};

/// Converts JACK's connected-port total latency into the shared
/// residual-plus-period representation without counting the period twice.
std::optional<AudioIoTiming>
make_jack_audio_io_timing(const JackTimingValues& values,
                          std::uint64_t route_instance_token,
                          std::uint64_t calibration_generation) noexcept;

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
    std::optional<AudioIoTiming> audio_io_timing() const noexcept;

private:
    static int process_callback(jack_nframes_t nframes, void* arg);
    static void shutdown_callback(void* arg);
    static void latency_callback(jack_latency_callback_mode_t mode, void* arg);
    void publish_latency(jack_latency_callback_mode_t mode) noexcept;
    void invalidate_audio_io_timing() noexcept;

    std::string client_name_;
    jack_client_t* client_ = nullptr;
    std::vector<jack_port_t*> output_ports_;
    std::vector<jack_port_t*> input_ports_;
    DeviceConfig config_;
    AudioCallback callback_;
    std::atomic<bool> is_running_{false};
    uint64_t sample_position_ = 0;
    std::atomic<std::uint64_t> timing_sequence_{0};
    std::atomic<std::uint64_t> route_instance_token_{0};
    std::atomic<std::uint64_t> calibration_generation_{0};
    std::atomic<std::uint32_t> input_total_latency_frames_{0};
    std::atomic<std::uint32_t> output_total_latency_frames_{0};
    std::atomic<std::uint32_t> period_frames_{0};
    std::atomic<std::uint32_t> timing_sample_rate_hz_{0};
    std::atomic<bool> has_input_timing_{false};
    std::atomic<bool> has_output_timing_{false};
    std::atomic<bool> timing_update_in_progress_{false};
    std::uint8_t staged_latency_modes_ = 0;
    bool staged_input_timing_ = false;
    bool staged_output_timing_ = false;
    std::uint32_t staged_input_total_latency_frames_ = 0;
    std::uint32_t staged_output_total_latency_frames_ = 0;
};

// Check if a JACK server is available
bool jack_is_available();

/// AudioSystem wrapper that routes create_device() to JackDevice when
/// the Linux factory selects a JACK-backed audio system.
class JackSystem : public AudioSystem {
public:
    std::vector<DeviceInfo> enumerate_devices() override;
    std::unique_ptr<AudioDevice> create_device(const std::string& device_id) override;
    DeviceInfo default_output_device() override;
    DeviceInfo default_input_device() override;
};

} // namespace pulp::audio::linux_platform

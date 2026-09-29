#pragma once

/// @file null_audio_device.hpp
/// An audio device that renders in real time and plays nothing.
///
/// A standalone app that must run its full audio graph (analyzers, meters,
/// a test signal feeding the input) while nothing reaches the speakers, for
/// example during a live-window performance measurement, selects this device
/// with `PULP_AUDIO_DEVICE=null`. No platform audio API is touched: the
/// callback runs on an ordinary thread paced to the configured sample rate
/// and block size, input buffers are silence (the standalone's test signal
/// replaces them when armed), and output is discarded.
///
/// Pacing uses absolute deadlines, so blocks are delivered at the real-time
/// rate on average even when a callback runs long; a callback that falls more
/// than a few blocks behind counts an xrun and resynchronises instead of
/// bursting to catch up, as a real device would drop the missed periods.

#include <pulp/audio/device.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace pulp::format::detail {

using audio::AudioCallback;
using audio::AudioDevice;
using audio::AudioSystem;
using audio::DeviceConfig;
using audio::DeviceInfo;

/// Device id and system name the null device reports.
inline constexpr const char* kNullAudioDeviceId = "null";

class NullAudioDevice final : public AudioDevice {
public:
    NullAudioDevice() = default;
    ~NullAudioDevice() override;

    bool open(const DeviceConfig& config) override;
    void close() override;
    bool start(AudioCallback callback) override;
    void stop() override;

    bool is_open() const override { return open_; }
    bool is_running() const override { return running_.load(std::memory_order_acquire); }
    DeviceInfo info() const override;
    double sample_rate() const override { return config_.sample_rate; }
    int buffer_size() const override { return config_.buffer_size; }
    std::uint64_t xrun_count() const override { return xruns_.load(std::memory_order_relaxed); }
    void reset_xrun_counter() override { xruns_.store(0, std::memory_order_relaxed); }

    /// Blocks delivered to the callback since start().
    std::uint64_t blocks_rendered() const { return blocks_.load(std::memory_order_relaxed); }

private:
    void run();

    DeviceConfig config_{};
    bool open_ = false;
    AudioCallback callback_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> blocks_{0};
    std::atomic<std::uint64_t> xruns_{0};
    std::thread thread_;
    std::vector<std::vector<float>> input_;
    std::vector<std::vector<float>> output_;
};

class NullAudioSystem final : public AudioSystem {
public:
    std::vector<DeviceInfo> enumerate_devices() override;
    std::unique_ptr<AudioDevice> create_device(const std::string& device_id = "") override;
    DeviceInfo default_output_device() override;
    DeviceInfo default_input_device() override;
};

std::unique_ptr<AudioSystem> create_null_audio_system();

/// Whether a `PULP_AUDIO_DEVICE` value selects the null device ("null",
/// case-insensitive). Absent or anything else keeps the platform device.
bool null_audio_device_requested(const char* env_value) noexcept;

} // namespace pulp::format::detail

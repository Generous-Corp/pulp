#include "alsa_device.hpp"
#include <pulp/runtime/log.hpp>

#include <cstring>
#include <algorithm>

namespace pulp::audio::linux_platform {

// ── AlsaDevice ───────────────────────────────────────────────────────────

AlsaDevice::AlsaDevice(const std::string& device_name)
    : device_name_(device_name)
{
}

AlsaDevice::~AlsaDevice() {
    if (is_running_.load()) stop();
    if (is_open_) close();
}

bool AlsaDevice::open(const DeviceConfig& config) {
    config_ = config;

    int err = snd_pcm_open(&pcm_, device_name_.c_str(),
        SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        runtime::log_error("ALSA: could not open device '{}': {}",
            device_name_, snd_strerror(err));
        return false;
    }

    // Configure hardware parameters
    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);
    snd_pcm_hw_params_any(pcm_, hw_params);

    // Interleaved float32
    err = snd_pcm_hw_params_set_access(pcm_, hw_params,
        SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) {
        runtime::log_error("ALSA: could not set interleaved access: {}", snd_strerror(err));
        close();
        return false;
    }

    err = snd_pcm_hw_params_set_format(pcm_, hw_params, SND_PCM_FORMAT_FLOAT_LE);
    if (err < 0) {
        runtime::log_error("ALSA: could not set float format: {}", snd_strerror(err));
        close();
        return false;
    }

    // Channels
    actual_channels_ = config_.output_channels;
    err = snd_pcm_hw_params_set_channels(pcm_, hw_params,
        static_cast<unsigned>(actual_channels_));
    if (err < 0) {
        // Try stereo fallback
        actual_channels_ = 2;
        err = snd_pcm_hw_params_set_channels(pcm_, hw_params, 2);
        if (err < 0) {
            runtime::log_error("ALSA: could not set channels: {}", snd_strerror(err));
            close();
            return false;
        }
    }

    // Sample rate
    unsigned int rate = static_cast<unsigned int>(config_.sample_rate);
    err = snd_pcm_hw_params_set_rate_near(pcm_, hw_params, &rate, nullptr);
    if (err < 0) {
        runtime::log_error("ALSA: could not set sample rate: {}", snd_strerror(err));
        close();
        return false;
    }
    config_.sample_rate = static_cast<double>(rate);

    // Period size (buffer size per callback)
    period_size_ = static_cast<snd_pcm_uframes_t>(config_.buffer_size);
    err = snd_pcm_hw_params_set_period_size_near(pcm_, hw_params,
        &period_size_, nullptr);
    if (err < 0) {
        runtime::log_error("ALSA: could not set period size: {}", snd_strerror(err));
        close();
        return false;
    }
    config_.buffer_size = static_cast<int>(period_size_);

    // Buffer size = 2 periods (double buffering)
    snd_pcm_uframes_t buffer_size = period_size_ * 2;
    err = snd_pcm_hw_params_set_buffer_size_near(pcm_, hw_params, &buffer_size);
    if (err < 0) {
        runtime::log_error("ALSA: could not set buffer size: {}", snd_strerror(err));
        close();
        return false;
    }

    // Apply hardware parameters
    err = snd_pcm_hw_params(pcm_, hw_params);
    if (err < 0) {
        runtime::log_error("ALSA: could not apply hw params: {}", snd_strerror(err));
        close();
        return false;
    }

    // Pre-allocate buffers
    channel_buffers_.resize(actual_channels_);
    output_ptrs_.resize(actual_channels_);
    for (int ch = 0; ch < actual_channels_; ++ch) {
        channel_buffers_[ch].resize(period_size_, 0.0f);
        output_ptrs_[ch] = channel_buffers_[ch].data();
    }
    interleaved_.resize(period_size_ * actual_channels_, 0.0f);

    is_open_ = true;
    runtime::log_info("ALSA: opened '{}' at {} Hz, period {} frames, {} channels",
        device_name_, config_.sample_rate, period_size_, actual_channels_);
    return true;
}

void AlsaDevice::close() {
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    channel_buffers_.clear();
    output_ptrs_.clear();
    interleaved_.clear();
    is_open_ = false;
}

bool AlsaDevice::start(AudioCallback callback) {
    if (!is_open_) return false;
    callback_ = std::move(callback);
    sample_position_ = 0;

    is_running_.store(true, std::memory_order_release);
    render_thread_ = std::thread([this] { render_thread_func(); });

    return true;
}

void AlsaDevice::stop() {
    if (!is_running_.load(std::memory_order_acquire)) return;

    is_running_.store(false, std::memory_order_release);

    if (render_thread_.joinable()) {
        render_thread_.join();
    }

    if (pcm_) {
        snd_pcm_drain(pcm_);
    }

    callback_ = nullptr;
}

DeviceInfo AlsaDevice::info() const {
    DeviceInfo info;
    info.id = device_name_;
    info.name = device_name_;
    if (device_name_ == "default" || device_name_ == "pulse") {
        info.is_default_output = true;
        info.is_default_input = true;
    }
    info.max_output_channels = actual_channels_ > 0 ? actual_channels_ : 2;
    info.max_input_channels = 2;
    info.sample_rates = {44100, 48000, 88200, 96000};
    info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
    return info;
}

void AlsaDevice::render_thread_func() {
    // Set thread scheduling priority if possible (requires CAP_SYS_NICE or rtkit)
    // Not critical — ALSA write will block naturally

    while (is_running_.load(std::memory_order_relaxed)) {
        if (callback_) {
            // Clear buffers
            for (int ch = 0; ch < actual_channels_; ++ch) {
                std::memset(output_ptrs_[ch], 0, period_size_ * sizeof(float));
            }

            // Call the user callback with non-interleaved buffers
            BufferView<const float> input_view;
            BufferView<float> output_view(output_ptrs_.data(),
                static_cast<size_t>(actual_channels_), period_size_);

            CallbackContext ctx;
            ctx.sample_rate = config_.sample_rate;
            ctx.buffer_size = static_cast<int>(period_size_);
            ctx.sample_position = sample_position_;

            callback_(input_view, output_view, ctx);

            // Interleave for ALSA
            for (snd_pcm_uframes_t frame = 0; frame < period_size_; ++frame) {
                for (int ch = 0; ch < actual_channels_; ++ch) {
                    interleaved_[frame * actual_channels_ + ch] = output_ptrs_[ch][frame];
                }
            }
        } else {
            std::memset(interleaved_.data(), 0,
                period_size_ * actual_channels_ * sizeof(float));
        }

        // Write to ALSA (blocking)
        snd_pcm_sframes_t frames = snd_pcm_writei(pcm_,
            interleaved_.data(), period_size_);

        if (frames < 0) {
            // Recover from underrun or other errors
            frames = snd_pcm_recover(pcm_, static_cast<int>(frames), 1);
            if (frames < 0) {
                runtime::log_error("ALSA: write failed: {}", snd_strerror(static_cast<int>(frames)));
                break;
            }
        }

        sample_position_ += static_cast<uint64_t>(frames);
    }
}

// ── AlsaSystem ───────────────────────────────────────────────────────────

std::vector<DeviceInfo> AlsaSystem::enumerate_devices() {
    std::vector<DeviceInfo> devices;

    // Always include "default" and "pulse" (PulseAudio/PipeWire)
    {
        DeviceInfo info;
        info.id = "default";
        info.name = "Default";
        info.max_output_channels = 2;
        info.max_input_channels = 2;
        info.is_default_output = true;
        info.is_default_input = true;
        info.sample_rates = {44100, 48000, 88200, 96000};
        info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
        devices.push_back(std::move(info));
    }

    // Enumerate hardware devices
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char* name = nullptr;
        if (snd_card_get_name(card, &name) == 0 && name) {
            DeviceInfo info;
            info.id = "hw:" + std::to_string(card);
            info.name = name;
            info.max_output_channels = 2;
            info.max_input_channels = 2;
            info.sample_rates = {44100, 48000, 88200, 96000};
            info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
            devices.push_back(std::move(info));
            free(name);
        }
    }

    return devices;
}

std::unique_ptr<AudioDevice> AlsaSystem::create_device(const std::string& device_id) {
    std::string name = device_id.empty() ? "default" : device_id;
    return std::make_unique<AlsaDevice>(name);
}

DeviceInfo AlsaSystem::default_output_device() {
    DeviceInfo info;
    info.id = "default";
    info.name = "Default";
    info.max_output_channels = 2;
    info.is_default_output = true;
    info.sample_rates = {44100, 48000, 88200, 96000};
    info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
    return info;
}

DeviceInfo AlsaSystem::default_input_device() {
    DeviceInfo info;
    info.id = "default";
    info.name = "Default";
    info.max_input_channels = 2;
    info.is_default_input = true;
    info.sample_rates = {44100, 48000, 88200, 96000};
    info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
    return info;
}

} // namespace pulp::audio::linux_platform

// Factory function — prefers JACK when available (lower latency, PipeWire compatible)
namespace pulp::audio {

#ifdef PULP_HAS_JACK
// Forward declaration — implemented in jack_device.cpp
namespace linux_platform { bool jack_is_available(); }
#endif

std::unique_ptr<AudioSystem> create_audio_system() {
    // ALSA is always the fallback — works everywhere including PipeWire/PulseAudio
    return std::make_unique<linux_platform::AlsaSystem>();
}

} // namespace pulp::audio

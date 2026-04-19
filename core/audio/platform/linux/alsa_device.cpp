#include "alsa_device.hpp"
#include <pulp/runtime/log.hpp>

#include <cstring>
#include <algorithm>

namespace pulp::audio::linux_platform {

// ── AlsaDevice ───────────────────────────────────────────────────────────

AlsaDevice::AlsaDevice(const std::string& device_name,
                       snd_pcm_stream_t stream)
    : device_name_(device_name), stream_(stream)
{
}

AlsaDevice::~AlsaDevice() {
    if (is_running_.load()) stop();
    if (is_open_) close();
}

bool AlsaDevice::open(const DeviceConfig& config) {
    config_ = config;

    int err = snd_pcm_open(&pcm_, device_name_.c_str(), stream_, 0);
    if (err < 0) {
        runtime::log_error("ALSA: could not open {} device '{}': {}",
            stream_ == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
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

    // Channels — pick the budget based on direction.
    actual_channels_ = (stream_ == SND_PCM_STREAM_CAPTURE)
        ? std::max(config_.input_channels, 1)
        : std::max(config_.output_channels, 1);
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
    channel_ptrs_.resize(actual_channels_);
    for (int ch = 0; ch < actual_channels_; ++ch) {
        channel_buffers_[ch].resize(period_size_, 0.0f);
        channel_ptrs_[ch] = channel_buffers_[ch].data();
    }
    interleaved_.resize(period_size_ * actual_channels_, 0.0f);

    is_open_ = true;
    runtime::log_info("ALSA: opened {} '{}' at {} Hz, period {} frames, {} channels",
        stream_ == SND_PCM_STREAM_CAPTURE ? "capture" : "playback",
        device_name_, config_.sample_rate, period_size_, actual_channels_);
    return true;
}

void AlsaDevice::close() {
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    channel_buffers_.clear();
    channel_ptrs_.clear();
    interleaved_.clear();
    is_open_ = false;
}

bool AlsaDevice::start(AudioCallback callback) {
    if (!is_open_) return false;
    callback_ = std::move(callback);
    sample_position_ = 0;

    is_running_.store(true, std::memory_order_release);
    io_thread_ = std::thread([this] {
        if (stream_ == SND_PCM_STREAM_CAPTURE) capture_thread_func();
        else                                   render_thread_func();
    });

    return true;
}

void AlsaDevice::stop() {
    if (!is_running_.load(std::memory_order_acquire)) return;

    is_running_.store(false, std::memory_order_release);

    // On capture we MUST drop BEFORE joining — snd_pcm_readi blocks
    // inside the io thread until frames arrive, so the thread never
    // reaches its loop-check on a quiet input device. snd_pcm_drop
    // aborts any outstanding read from this side of the stream so the
    // thread unwinds immediately. On playback the existing join-then-
    // drain order is fine because the render thread exits on its own
    // fill cycle. See #438 P1 Codex review on #387.
    if (pcm_ && stream_ == SND_PCM_STREAM_CAPTURE) {
        snd_pcm_drop(pcm_);
    }

    if (io_thread_.joinable()) io_thread_.join();

    if (pcm_) {
        // Drain only on playback — drain on capture blocks until the
        // ring buffer empties, which is meaningless for input. Capture
        // was already dropped above before join.
        if (stream_ == SND_PCM_STREAM_PLAYBACK) snd_pcm_drain(pcm_);
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
    // Channel count reflects the direction this device was opened in.
    if (stream_ == SND_PCM_STREAM_CAPTURE) {
        info.max_input_channels  = actual_channels_ > 0 ? actual_channels_ : 2;
        info.max_output_channels = 0;
    } else {
        info.max_output_channels = actual_channels_ > 0 ? actual_channels_ : 2;
        info.max_input_channels  = 0;
    }
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
                std::memset(channel_ptrs_[ch], 0, period_size_ * sizeof(float));
            }

            // Call the user callback with non-interleaved buffers
            BufferView<const float> input_view;
            BufferView<float> output_view(channel_ptrs_.data(),
                static_cast<size_t>(actual_channels_), period_size_);

            CallbackContext ctx;
            ctx.sample_rate = config_.sample_rate;
            ctx.buffer_size = static_cast<int>(period_size_);
            ctx.sample_position = sample_position_;

            callback_(input_view, output_view, ctx);

            // Interleave for ALSA
            for (snd_pcm_uframes_t frame = 0; frame < period_size_; ++frame) {
                for (int ch = 0; ch < actual_channels_; ++ch) {
                    interleaved_[frame * actual_channels_ + ch] = channel_ptrs_[ch][frame];
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

// ── Capture thread (#20 / #215) ─────────────────────────────────────
//
// snd_pcm_readi blocks until period_size frames are available. We
// recover from xruns the same way render does and hand the user a
// populated input BufferView<const float> with empty output. Callers
// that want duplex open both an input AND an output AlsaDevice and
// route input → output themselves (same model as WASAPI).
void AlsaDevice::capture_thread_func() {
    while (is_running_.load(std::memory_order_relaxed)) {
        // Read a period of frames from ALSA (blocking)
        snd_pcm_sframes_t frames = snd_pcm_readi(pcm_,
            interleaved_.data(), period_size_);

        if (frames < 0) {
            frames = snd_pcm_recover(pcm_, static_cast<int>(frames), 1);
            if (frames < 0) {
                runtime::log_error("ALSA: read failed: {}",
                    snd_strerror(static_cast<int>(frames)));
                break;
            }
            continue;
        }

        const auto got = static_cast<snd_pcm_uframes_t>(frames);

        // Deinterleave into planar channel buffers
        for (snd_pcm_uframes_t frame = 0; frame < got; ++frame) {
            for (int ch = 0; ch < actual_channels_; ++ch) {
                channel_ptrs_[ch][frame] =
                    interleaved_[frame * actual_channels_ + ch];
            }
        }

        if (callback_) {
            BufferView<const float> input_view(
                const_cast<const float**>(channel_ptrs_.data()),
                static_cast<size_t>(actual_channels_), got);
            BufferView<float> output_view;  // capture-only

            CallbackContext ctx;
            ctx.sample_rate = config_.sample_rate;
            ctx.buffer_size = static_cast<int>(got);
            ctx.sample_position = sample_position_;

            callback_(input_view, output_view, ctx);
        }

        sample_position_ += got;
    }
}

// ── AlsaSystem ───────────────────────────────────────────────────────────

namespace {

// Probe an ALSA device for its real per-direction channel maximum by
// briefly opening it in non-blocking mode and reading hw_params. Falls
// back to 0 if the device can't be probed (busy, missing, no support
// for that direction). #20 — replaces the hardcoded `2` placeholder.
unsigned probe_max_channels(const std::string& id, snd_pcm_stream_t stream) {
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, id.c_str(), stream, SND_PCM_NONBLOCK) < 0) {
        return 0;
    }

    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);
    unsigned max_ch = 0;
    if (snd_pcm_hw_params_any(pcm, hw_params) >= 0) {
        snd_pcm_hw_params_get_channels_max(hw_params, &max_ch);
    }
    snd_pcm_close(pcm);
    return max_ch;
}

void fill_real_channel_counts(DeviceInfo& info) {
    const auto out_ch = probe_max_channels(info.id, SND_PCM_STREAM_PLAYBACK);
    const auto in_ch  = probe_max_channels(info.id, SND_PCM_STREAM_CAPTURE);
    if (out_ch > 0) info.max_output_channels = static_cast<int>(out_ch);
    if (in_ch  > 0) info.max_input_channels  = static_cast<int>(in_ch);
    // Fallbacks if probing failed entirely (e.g. CI runners with no
    // audio at all) — keep the previous "stereo" placeholder so
    // callers don't see 0 and bail out.
    if (info.max_output_channels == 0 && info.max_input_channels == 0) {
        info.max_output_channels = 2;
        info.max_input_channels  = 2;
    }
}

}  // namespace

std::vector<DeviceInfo> AlsaSystem::enumerate_devices() {
    std::vector<DeviceInfo> devices;

    // Always include "default" (and PulseAudio/PipeWire if it routes
    // through ALSA's "pulse" plugin). Probe per-direction channel
    // counts so consumers don't see the old hardcoded 2.
    {
        DeviceInfo info;
        info.id = "default";
        info.name = "Default";
        info.is_default_output = true;
        info.is_default_input = true;
        info.sample_rates = {44100, 48000, 88200, 96000};
        info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
        fill_real_channel_counts(info);
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
            info.sample_rates = {44100, 48000, 88200, 96000};
            info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
            fill_real_channel_counts(info);
            devices.push_back(std::move(info));
            free(name);
        }
    }

    return devices;
}

std::unique_ptr<AudioDevice> AlsaSystem::create_device(const std::string& device_id) {
    // Pick the stream direction by probing what the device actually
    // supports. If the requested device has only an input endpoint we
    // open it as CAPTURE; otherwise default to PLAYBACK (preserves
    // the old behaviour for output-only callers and explicit hw:N ids
    // that have both).
    std::string name = device_id.empty() ? "default" : device_id;
    snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;
    const auto out_ch = probe_max_channels(name, SND_PCM_STREAM_PLAYBACK);
    const auto in_ch  = probe_max_channels(name, SND_PCM_STREAM_CAPTURE);
    if (out_ch == 0 && in_ch > 0) {
        stream = SND_PCM_STREAM_CAPTURE;
    }
    return std::make_unique<AlsaDevice>(name, stream);
}

DeviceInfo AlsaSystem::default_output_device() {
    DeviceInfo info;
    info.id = "default";
    info.name = "Default";
    info.is_default_output = true;
    info.sample_rates = {44100, 48000, 88200, 96000};
    info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
    fill_real_channel_counts(info);
    return info;
}

DeviceInfo AlsaSystem::default_input_device() {
    DeviceInfo info;
    info.id = "default";
    info.name = "Default";
    info.is_default_input = true;
    info.sample_rates = {44100, 48000, 88200, 96000};
    info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
    fill_real_channel_counts(info);
    return info;
}

} // namespace pulp::audio::linux_platform

#ifdef PULP_HAS_JACK
#include "jack_device.hpp"  // JackSystem + jack_is_available
#endif

// Factory function — prefers JACK when a running server is detected.
// Workstream 02 slice 2.2: previously unconditionally returned AlsaSystem
// even when the JACK backend was compiled in, so JACK was dead code.
namespace pulp::audio {

std::unique_ptr<AudioSystem> create_audio_system() {
#ifdef PULP_HAS_JACK
    if (linux_platform::jack_is_available()) {
        return std::make_unique<linux_platform::JackSystem>();
    }
#endif
    // ALSA fallback — works everywhere including PipeWire/PulseAudio.
    return std::make_unique<linux_platform::AlsaSystem>();
}

} // namespace pulp::audio

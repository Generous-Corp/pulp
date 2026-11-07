#include "alsa_device.hpp"
#include <pulp/runtime/log.hpp>

#include <cstring>
#include <algorithm>
#include <limits>

namespace pulp::audio::linux_platform {

std::optional<AudioIoTiming>
make_alsa_audio_io_timing(const AlsaTimingValues& values,
                          std::uint64_t route_instance_token,
                          std::uint64_t calibration_generation) noexcept {
    if (values.route_delay_frames < 0 || values.period_frames == 0 ||
        values.period_frames > std::numeric_limits<std::uint32_t>::max() ||
        !is_supported_audio_sample_rate(values.sample_rate_hz) ||
        route_instance_token == 0 || calibration_generation == 0) {
        return std::nullopt;
    }

    const auto period = static_cast<std::uint64_t>(values.period_frames);
    const auto complete_delay = std::max(
        static_cast<std::uint64_t>(values.route_delay_frames), period);
    const auto residual = complete_delay - period;
    if (residual > std::numeric_limits<std::uint32_t>::max())
        return std::nullopt;

    AudioIoTiming timing{};
    if (values.direction == SND_PCM_STREAM_CAPTURE) {
        timing.input_latency_frames = static_cast<std::uint32_t>(residual);
        timing.input_safety_offset_frames = 0;
    } else if (values.direction == SND_PCM_STREAM_PLAYBACK) {
        timing.output_latency_frames = static_cast<std::uint32_t>(residual);
        timing.output_safety_offset_frames = 0;
    } else {
        return std::nullopt;
    }
    timing.io_buffer_frames = static_cast<std::uint32_t>(period);
    timing.sample_rate_hz = values.sample_rate_hz;
    timing.timestamp_domain = AudioTimestampDomain::monotonic_host_time;
    timing.timestamp_source = AudioTimingSource::device_clock;
    timing.confidence = AudioTimingConfidence::reported;
    timing.route_instance_token = route_instance_token;
    timing.calibration_generation = calibration_generation;
    return timing;
}

// ── Driver capability probing ────────────────────────────────────────────

namespace {

// Read what the driver allows from a hw_params space that has had its access
// and format chosen. Rates are confirmed one by one with
// snd_pcm_hw_params_test_rate, because a driver reports a continuous min..max
// that often contains rates it cannot actually run.
PcmCapabilities read_pcm_capabilities(snd_pcm_t* pcm, snd_pcm_hw_params_t* params) {
    PcmCapabilities caps;
    int dir = 0;
    unsigned v = 0;

    if (snd_pcm_hw_params_get_rate_min(params, &v, &dir) >= 0)
        caps.rate_min = v;
    if (snd_pcm_hw_params_get_rate_max(params, &v, &dir) >= 0)
        caps.rate_max = v;
    caps.rates_tested = true;
    for (unsigned rate : standard_sample_rates()) {
        if (rate < caps.rate_min || rate > caps.rate_max)
            continue;
        if (snd_pcm_hw_params_test_rate(pcm, params, rate, 0) == 0) {
            caps.accepted_rates.push_back(rate);
        }
    }
    // A driver with one fixed rate may not be a standard one.
    if (caps.rate_min != 0 && caps.rate_min == caps.rate_max &&
        snd_pcm_hw_params_test_rate(pcm, params, caps.rate_min, 0) == 0 &&
        std::find(caps.accepted_rates.begin(), caps.accepted_rates.end(), caps.rate_min) ==
            caps.accepted_rates.end()) {
        caps.accepted_rates.push_back(caps.rate_min);
    }

    if (snd_pcm_hw_params_get_channels_min(params, &v) >= 0)
        caps.channels_min = v;
    if (snd_pcm_hw_params_get_channels_max(params, &v) >= 0)
        caps.channels_max = v;

    snd_pcm_uframes_t frames = 0;
    if (snd_pcm_hw_params_get_period_size_min(params, &frames, &dir) >= 0)
        caps.period_min = frames;
    if (snd_pcm_hw_params_get_period_size_max(params, &frames, &dir) >= 0)
        caps.period_max = frames;
    if (snd_pcm_hw_params_get_buffer_size_max(params, &frames) >= 0)
        caps.buffer_max = frames;
    return caps;
}

// Open the device briefly in non-blocking mode and read its capabilities for
// one direction. Empty when the device cannot be opened for that direction
// (busy, missing, or no such direction).
std::optional<PcmCapabilities> probe_pcm_capabilities(const std::string& id,
                                                      snd_pcm_stream_t stream) {
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, id.c_str(), stream, SND_PCM_NONBLOCK) < 0)
        return std::nullopt;

    snd_pcm_hw_params_t* hw_params = nullptr;
    snd_pcm_hw_params_alloca(&hw_params);
    std::optional<PcmCapabilities> caps;
    if (snd_pcm_hw_params_any(pcm, hw_params) >= 0 &&
        snd_pcm_hw_params_set_access(pcm, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED) >= 0) {
        // The stream is opened as float; if the hardware cannot do float
        // natively, report the widest space rather than nothing.
        snd_pcm_hw_params_t* with_format = nullptr;
        snd_pcm_hw_params_alloca(&with_format);
        snd_pcm_hw_params_copy(with_format, hw_params);
        if (snd_pcm_hw_params_set_format(pcm, with_format, SND_PCM_FORMAT_FLOAT_LE) >= 0) {
            caps = read_pcm_capabilities(pcm, with_format);
        } else {
            caps = read_pcm_capabilities(pcm, hw_params);
        }
    }
    snd_pcm_close(pcm);
    return caps;
}

// Fill rates, buffer sizes and channel counts for a device id from the driver.
void fill_probed_capabilities(DeviceInfo& info) {
    fill_device_info_from_capabilities(info,
                                       probe_pcm_capabilities(info.id, SND_PCM_STREAM_PLAYBACK),
                                       probe_pcm_capabilities(info.id, SND_PCM_STREAM_CAPTURE));
}

} // namespace

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
    invalidate_audio_io_timing();
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

    capabilities_ = read_pcm_capabilities(pcm_, hw_params);

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

    snd_pcm_sw_params_t* sw_params = nullptr;
    snd_pcm_sw_params_alloca(&sw_params);
    monotonic_timing_available_.store(
        snd_pcm_sw_params_current(pcm_, sw_params) >= 0 &&
        snd_pcm_sw_params_set_tstamp_mode(
            pcm_, sw_params, SND_PCM_TSTAMP_ENABLE) >= 0 &&
        snd_pcm_sw_params_set_tstamp_type(
            pcm_, sw_params, SND_PCM_TSTAMP_TYPE_MONOTONIC) >= 0 &&
        snd_pcm_sw_params(pcm_, sw_params) >= 0,
        std::memory_order_release);

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
    invalidate_audio_io_timing();
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_ = nullptr;
    }
    channel_buffers_.clear();
    channel_ptrs_.clear();
    interleaved_.clear();
    capabilities_.reset();
    is_open_ = false;
}

void AlsaDevice::invalidate_audio_io_timing() noexcept {
    route_instance_token_.store(0, std::memory_order_release);
    calibration_generation_.store(0, std::memory_order_release);
    last_reported_delay_frames_.store(-1, std::memory_order_release);
    monotonic_timing_available_.store(false, std::memory_order_release);
}

std::optional<AudioIoTiming> AlsaDevice::audio_io_timing() const noexcept {
    const auto token = route_instance_token_.load(std::memory_order_acquire);
    auto generation = calibration_generation_.load(std::memory_order_acquire);
    if (!is_open_ || !pcm_ || !is_running_.load(std::memory_order_acquire) ||
        !monotonic_timing_available_.load(std::memory_order_acquire) || token == 0 ||
        generation == 0 || snd_pcm_state(pcm_) != SND_PCM_STATE_RUNNING) {
        return std::nullopt;
    }

    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(pcm_, &delay) < 0)
        return std::nullopt;
    const auto previous =
        last_reported_delay_frames_.exchange(delay, std::memory_order_acq_rel);
    if (previous >= 0 && previous != delay) {
        const auto next = next_calibration_generation(generation);
        if (!next) {
            calibration_generation_.store(0, std::memory_order_release);
            return std::nullopt;
        }
        generation = *next;
        calibration_generation_.store(generation, std::memory_order_release);
    }
    if (route_instance_token_.load(std::memory_order_acquire) != token ||
        calibration_generation_.load(std::memory_order_acquire) != generation) {
        return std::nullopt;
    }
    return make_alsa_audio_io_timing(
        {delay, period_size_, config_.sample_rate, stream_}, token, generation);
}

bool AlsaDevice::start(AudioCallback callback) {
    if (!is_open_) return false;
    callback_ = std::move(callback);
    sample_position_ = 0;
    last_reported_delay_frames_.store(-1, std::memory_order_release);
    calibration_generation_.store(1, std::memory_order_release);
    route_instance_token_.store(
        detail::next_linux_audio_route_instance_token(), std::memory_order_release);

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
    // fill cycle.
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
    route_instance_token_.store(0, std::memory_order_release);
    calibration_generation_.store(0, std::memory_order_release);
    last_reported_delay_frames_.store(-1, std::memory_order_release);
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
    // Rates and buffer sizes come from the driver, read when the device was
    // opened; before that, probe it now.
    const auto caps = capabilities_ ? capabilities_ : probe_pcm_capabilities(device_name_, stream_);
    if (caps) {
        info.sample_rates = sample_rates_from(*caps);
        info.buffer_sizes = buffer_sizes_from(*caps);
    }
    if (info.sample_rates.empty() || info.buffer_sizes.empty()) {
        DeviceInfo fallback;
        fill_device_info_from_capabilities(fallback, std::nullopt, std::nullopt);
        if (info.sample_rates.empty())
            info.sample_rates = fallback.sample_rates;
        if (info.buffer_sizes.empty())
            info.buffer_sizes = fallback.buffer_sizes;
    }
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
                invalidate_audio_io_timing();
                break;
            }
        }

        sample_position_ += static_cast<uint64_t>(frames);
    }
}

// ── Capture thread ──────────────────────────────────────────────────
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
                invalidate_audio_io_timing();
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
        fill_probed_capabilities(info);
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
            fill_probed_capabilities(info);
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
    // the old behavior for output-only callers and explicit hw:N ids
    // that have both).
    std::string name = device_id.empty() ? "default" : device_id;
    snd_pcm_stream_t stream = SND_PCM_STREAM_PLAYBACK;

    // Do not infer capture-only from a failed capability probe: a probe
    // fails for ANY open error, including transient busy-device errors, and
    // on a playback-capable card whose playback probe fails just once that
    // would silently flip the device into capture mode and hand output
    // callers a useless capture stream.
    //
    // Distinguish "open failed" (could be transient) from "opened but
    // reports 0 channels" (definitive not-supported) by re-opening
    // for playback explicitly. Only downgrade to capture when playback
    // open actually fails with a stable error (ENODEV, ENOENT) and
    // capture succeeds. For anything else, leave the default PLAYBACK
    // — the subsequent start() call will surface a clear error if the
    // device truly can't play back.
    auto open_direction = [](const std::string& id,
                             snd_pcm_stream_t s) -> int {
        snd_pcm_t* pcm = nullptr;
        int err = snd_pcm_open(&pcm, id.c_str(), s, SND_PCM_NONBLOCK);
        if (pcm) snd_pcm_close(pcm);
        return err;
    };
    const int play_err = open_direction(name, SND_PCM_STREAM_PLAYBACK);
    if (play_err == -ENODEV || play_err == -ENOENT) {
        const int cap_err = open_direction(name, SND_PCM_STREAM_CAPTURE);
        if (cap_err == 0) {
            stream = SND_PCM_STREAM_CAPTURE;
        }
    }
    return std::make_unique<AlsaDevice>(name, stream);
}

DeviceInfo AlsaSystem::default_output_device() {
    DeviceInfo info;
    info.id = "default";
    info.name = "Default";
    info.is_default_output = true;
    fill_probed_capabilities(info);
    return info;
}

DeviceInfo AlsaSystem::default_input_device() {
    DeviceInfo info;
    info.id = "default";
    info.name = "Default";
    info.is_default_input = true;
    fill_probed_capabilities(info);
    return info;
}

AlsaSystem::~AlsaSystem() {
    // Stop the monitor thread (which captures `this`) before the AudioSystem
    // base — and its callback slot — are torn down.
    hotplug_monitor_.stop();
}

void AlsaSystem::set_device_change_callback(DeviceChangeCallback cb) {
    const bool want_monitor = static_cast<bool>(cb);
    // Store/clear the callback in the base slot first.
    AudioSystem::set_device_change_callback(std::move(cb));
    if (want_monitor) {
        if (!hotplug_monitor_.running()) {
            // The "sound" subsystem covers ALSA PCM + raw-midi cards, so a
            // single monitor catches USB audio/MIDI add + remove. Honest no-op
            // if libudev is unavailable.
            hotplug_monitor_.start({"sound"}, [this](runtime::UdevChange) { fire_device_change(); });
        }
    } else {
        hotplug_monitor_.stop();
    }
}

} // namespace pulp::audio::linux_platform

#ifdef PULP_HAS_JACK
#include "jack_device.hpp"  // JackSystem + jack_is_available
#endif

// Factory function — prefers JACK when a running server is detected, then
// falls back to ALSA when JACK is unavailable or not compiled in.
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

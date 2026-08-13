#include "jack_device.hpp"
#include <pulp/runtime/log.hpp>

#include <cstring>
#include <algorithm>

namespace pulp::audio::linux_platform {

// JACK ports are registered one-per-channel and the device path hard-caps
// the count when opening (see JackDevice::open). enumerate_devices() must
// advertise the same ceiling so a host never asks for more channels than
// the device will actually create — keeping the two in lockstep via one
// constant prevents the advertise-64 / open-8 drift that previously made
// enumeration dishonest.
constexpr int kJackMaxChannels = 8;

std::optional<AudioIoTiming>
make_jack_audio_io_timing(const JackTimingValues& values,
                          std::uint64_t route_instance_token,
                          std::uint64_t calibration_generation) noexcept {
    if (values.period_frames == 0 ||
        !is_supported_audio_sample_rate(values.sample_rate_hz) ||
        route_instance_token == 0 || calibration_generation == 0 ||
        (!values.input_total_latency_frames &&
         !values.output_total_latency_frames)) {
        return std::nullopt;
    }

    const auto residual = [period = values.period_frames](std::uint32_t total) {
        return total > period ? total - period : 0;
    };
    AudioIoTiming timing{};
    if (values.input_total_latency_frames) {
        timing.input_latency_frames = residual(*values.input_total_latency_frames);
        timing.input_safety_offset_frames = 0;
    }
    if (values.output_total_latency_frames) {
        timing.output_latency_frames = residual(*values.output_total_latency_frames);
        timing.output_safety_offset_frames = 0;
    }
    timing.io_buffer_frames = values.period_frames;
    timing.sample_rate_hz = values.sample_rate_hz;
    timing.timestamp_domain = AudioTimestampDomain::device_sample_frames;
    timing.timestamp_source = AudioTimingSource::device_clock;
    timing.confidence = AudioTimingConfidence::reported;
    timing.route_instance_token = route_instance_token;
    timing.calibration_generation = calibration_generation;
    return timing;
}

// ── JackDevice ───────────────────────────────────────────────────────────

JackDevice::JackDevice(const std::string& client_name)
    : client_name_(client_name)
{
}

JackDevice::~JackDevice() {
    if (is_running_.load()) stop();
    if (client_) close();
}

bool JackDevice::open(const DeviceConfig& config) {
    invalidate_audio_io_timing();
    staged_latency_modes_ = 0;
    staged_input_timing_ = false;
    staged_output_timing_ = false;
    config_ = config;

    // Open a JACK client connection
    jack_status_t status;
    client_ = jack_client_open(client_name_.c_str(), JackNoStartServer, &status);
    if (!client_) {
        runtime::log_error("JACK: could not connect to server (status 0x{:x})",
            static_cast<unsigned>(status));
        return false;
    }

    // Set callbacks
    jack_set_process_callback(client_, process_callback, this);
    jack_on_shutdown(client_, shutdown_callback, this);
    if (jack_set_latency_callback(client_, latency_callback, this) != 0) {
        runtime::log_error("JACK: could not register latency callback");
        close();
        return false;
    }

    // Register output ports
    int out_channels = std::min(config_.output_channels, kJackMaxChannels);
    output_ports_.resize(out_channels);
    for (int i = 0; i < out_channels; ++i) {
        std::string port_name = "output_" + std::to_string(i + 1);
        output_ports_[i] = jack_port_register(client_, port_name.c_str(),
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!output_ports_[i]) {
            runtime::log_error("JACK: could not register output port {}", i);
            close();
            return false;
        }
    }

    // Register input ports (if requested)
    int in_channels = std::min(config_.input_channels, kJackMaxChannels);
    input_ports_.resize(in_channels);
    for (int i = 0; i < in_channels; ++i) {
        std::string port_name = "input_" + std::to_string(i + 1);
        input_ports_[i] = jack_port_register(client_, port_name.c_str(),
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!input_ports_[i]) {
            runtime::log_error("JACK: could not register input port {}", i);
            close();
            return false;
        }
    }

    // Update config with JACK's actual sample rate and buffer size
    config_.sample_rate = static_cast<double>(jack_get_sample_rate(client_));
    config_.buffer_size = static_cast<int>(jack_get_buffer_size(client_));
    route_instance_token_.store(
        detail::next_linux_audio_route_instance_token(), std::memory_order_release);

    runtime::log_info("JACK: connected as '{}' at {} Hz, buffer {} frames, {} out / {} in",
        client_name_, config_.sample_rate, config_.buffer_size,
        out_channels, in_channels);
    return true;
}

void JackDevice::close() {
    if (client_) {
        if (is_running_.exchange(false, std::memory_order_acq_rel))
            jack_deactivate(client_);
        jack_client_close(client_);
        client_ = nullptr;
    }
    invalidate_audio_io_timing();
    staged_latency_modes_ = 0;
    output_ports_.clear();
    input_ports_.clear();
}

bool JackDevice::start(AudioCallback callback) {
    if (!client_) return false;
    callback_ = std::move(callback);
    sample_position_ = 0;

    // Activate the client — JACK will start calling process_callback
    int err = jack_activate(client_);
    if (err != 0) {
        runtime::log_error("JACK: could not activate client (error {})", err);
        return false;
    }

    is_running_.store(true, std::memory_order_release);

    // Auto-connect to system playback ports
    const char** playback_ports = jack_get_ports(client_, nullptr, nullptr,
        JackPortIsPhysical | JackPortIsInput);
    if (playback_ports) {
        for (size_t i = 0; i < output_ports_.size() && playback_ports[i]; ++i) {
            jack_connect(client_,
                jack_port_name(output_ports_[i]),
                playback_ports[i]);
        }
        jack_free(playback_ports);
    }

    // Auto-connect from system capture ports
    const char** capture_ports = jack_get_ports(client_, nullptr, nullptr,
        JackPortIsPhysical | JackPortIsOutput);
    if (capture_ports) {
        for (size_t i = 0; i < input_ports_.size() && capture_ports[i]; ++i) {
            jack_connect(client_,
                capture_ports[i],
                jack_port_name(input_ports_[i]));
        }
        jack_free(capture_ports);
    }

    // JACK only defines connected-port latency after graph connections exist.
    // Recompute requests the server-owned latency callbacks for this graph.
    if (jack_recompute_total_latencies(client_) != 0)
        invalidate_audio_io_timing();

    return true;
}

void JackDevice::stop() {
    if (!is_running_.load(std::memory_order_acquire)) return;

    is_running_.store(false, std::memory_order_release);

    if (client_) {
        jack_deactivate(client_);
    }

    callback_ = nullptr;
}

DeviceInfo JackDevice::info() const {
    DeviceInfo info;
    info.id = "jack";
    info.name = "JACK Audio";
    if (client_) {
        info.max_output_channels = static_cast<int>(output_ports_.size());
        info.max_input_channels = static_cast<int>(input_ports_.size());
        info.sample_rates.push_back(static_cast<double>(jack_get_sample_rate(client_)));
        info.buffer_sizes.push_back(static_cast<int>(jack_get_buffer_size(client_)));
    } else {
        info.max_output_channels = 2;
        info.max_input_channels = 2;
        info.sample_rates = {44100, 48000, 96000};
        info.buffer_sizes = {64, 128, 256, 512, 1024};
    }
    return info;
}

double JackDevice::sample_rate() const {
    if (client_) return static_cast<double>(jack_get_sample_rate(client_));
    return config_.sample_rate;
}

int JackDevice::buffer_size() const {
    if (client_) return static_cast<int>(jack_get_buffer_size(client_));
    return config_.buffer_size;
}

void JackDevice::invalidate_audio_io_timing() noexcept {
    timing_sequence_.fetch_add(1, std::memory_order_acq_rel);
    route_instance_token_.store(0, std::memory_order_relaxed);
    calibration_generation_.store(0, std::memory_order_relaxed);
    has_input_timing_.store(false, std::memory_order_relaxed);
    has_output_timing_.store(false, std::memory_order_relaxed);
    timing_update_in_progress_.store(false, std::memory_order_relaxed);
    timing_sequence_.fetch_add(1, std::memory_order_release);
}

std::optional<AudioIoTiming> JackDevice::audio_io_timing() const noexcept {
    if (timing_update_in_progress_.load(std::memory_order_acquire))
        return std::nullopt;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto before = timing_sequence_.load(std::memory_order_acquire);
        if ((before & 1U) != 0) continue;

        const auto token = route_instance_token_.load(std::memory_order_relaxed);
        const auto generation =
            calibration_generation_.load(std::memory_order_relaxed);
        JackTimingValues values{};
        if (has_input_timing_.load(std::memory_order_relaxed))
            values.input_total_latency_frames =
                input_total_latency_frames_.load(std::memory_order_relaxed);
        if (has_output_timing_.load(std::memory_order_relaxed))
            values.output_total_latency_frames =
                output_total_latency_frames_.load(std::memory_order_relaxed);
        values.period_frames = period_frames_.load(std::memory_order_relaxed);
        values.sample_rate_hz = static_cast<double>(
            timing_sample_rate_hz_.load(std::memory_order_relaxed));

        const auto after = timing_sequence_.load(std::memory_order_acquire);
        if (before == after &&
            !timing_update_in_progress_.load(std::memory_order_acquire)) {
            return make_jack_audio_io_timing(values, token, generation);
        }
    }
    return std::nullopt;
}

void JackDevice::latency_callback(jack_latency_callback_mode_t mode, void* arg) {
    static_cast<JackDevice*>(arg)->publish_latency(mode);
}

void JackDevice::publish_latency(jack_latency_callback_mode_t mode) noexcept {
    if (!client_) return;

    const auto token = route_instance_token_.load(std::memory_order_acquire);
    if (token == 0) {
        invalidate_audio_io_timing();
        return;
    }

    if (staged_latency_modes_ == 0) {
        timing_update_in_progress_.store(true, std::memory_order_release);
        staged_input_timing_ = false;
        staged_output_timing_ = false;
    }

    bool present = false;
    jack_latency_range_t aggregate{};
    const auto& ports = mode == JackCaptureLatency ? input_ports_ : output_ports_;
    for (auto* port : ports) {
        if (jack_port_connected(port) <= 0) continue;
        jack_latency_range_t range{};
        jack_port_get_latency_range(port, mode, &range);
        if (!present) {
            aggregate = range;
        } else {
            aggregate.min = std::min(aggregate.min, range.min);
            aggregate.max = std::max(aggregate.max, range.max);
        }
        present = true;
    }

    // A JACK latency callback must propagate the upstream range through this
    // zero-additional-cycle process client, not merely observe it locally.
    const auto& destinations =
        mode == JackCaptureLatency ? output_ports_ : input_ports_;
    for (auto* port : destinations)
        jack_port_set_latency_range(port, mode, &aggregate);

    if (mode == JackCaptureLatency) {
        staged_input_total_latency_frames_ = aggregate.max;
        staged_input_timing_ = present;
        staged_latency_modes_ |= 1U;
    } else {
        staged_output_total_latency_frames_ = aggregate.max;
        staged_output_timing_ = present;
        staged_latency_modes_ |= 2U;
    }
    if (staged_latency_modes_ != 3U)
        return;

    const auto current = calibration_generation_.load(std::memory_order_relaxed);
    const auto next = next_calibration_generation(current);
    if (!next) {
        invalidate_audio_io_timing();
        return;
    }
    timing_sequence_.fetch_add(1, std::memory_order_acq_rel);
    period_frames_.store(jack_get_buffer_size(client_), std::memory_order_relaxed);
    timing_sample_rate_hz_.store(jack_get_sample_rate(client_), std::memory_order_relaxed);
    input_total_latency_frames_.store(
        staged_input_total_latency_frames_, std::memory_order_relaxed);
    output_total_latency_frames_.store(
        staged_output_total_latency_frames_, std::memory_order_relaxed);
    has_input_timing_.store(staged_input_timing_, std::memory_order_relaxed);
    has_output_timing_.store(staged_output_timing_, std::memory_order_relaxed);
    calibration_generation_.store(*next, std::memory_order_relaxed);
    timing_sequence_.fetch_add(1, std::memory_order_release);
    staged_latency_modes_ = 0;
    timing_update_in_progress_.store(false, std::memory_order_release);
}

int JackDevice::process_callback(jack_nframes_t nframes, void* arg) {
    auto* self = static_cast<JackDevice*>(arg);
    if (!self->callback_ || !self->is_running_.load(std::memory_order_relaxed)) {
        // Silence all outputs
        for (auto* port : self->output_ports_) {
            auto* buf = static_cast<float*>(jack_port_get_buffer(port, nframes));
            std::memset(buf, 0, nframes * sizeof(float));
        }
        return 0;
    }

    // Get JACK buffer pointers — already non-interleaved, zero-copy
    float* out_ptrs[8] = {};
    const float* in_ptrs[8] = {};

    for (size_t i = 0; i < self->output_ports_.size() && i < 8; ++i) {
        out_ptrs[i] = static_cast<float*>(
            jack_port_get_buffer(self->output_ports_[i], nframes));
    }
    for (size_t i = 0; i < self->input_ports_.size() && i < 8; ++i) {
        in_ptrs[i] = static_cast<const float*>(
            jack_port_get_buffer(self->input_ports_[i], nframes));
    }

    BufferView<const float> input(in_ptrs, self->input_ports_.size(), nframes);
    BufferView<float> output(out_ptrs, self->output_ports_.size(), nframes);

    CallbackContext ctx;
    ctx.sample_rate = static_cast<double>(jack_get_sample_rate(self->client_));
    ctx.buffer_size = static_cast<int>(nframes);
    ctx.sample_position = self->sample_position_;

    self->callback_(input, output, ctx);
    self->sample_position_ += nframes;

    return 0;
}

void JackDevice::shutdown_callback(void* arg) {
    auto* self = static_cast<JackDevice*>(arg);
    self->is_running_.store(false, std::memory_order_release);
    self->invalidate_audio_io_timing();
    self->client_ = nullptr;  // JACK has already closed the client
    runtime::log_warn("JACK: server shut down");
}

bool jack_is_available() {
    jack_status_t status;
    jack_client_t* client = jack_client_open("pulp_probe", JackNoStartServer, &status);
    if (client) {
        jack_client_close(client);
        return true;
    }
    return false;
}

// ── JackSystem device enumeration ─────────────────────────────────────

std::vector<DeviceInfo> JackSystem::enumerate_devices() {
    // JACK exposes a single logical "server" as the device. Sample rate
    // and buffer size come from the running server; probe once to read
    // real values rather than hard-coded guesses.
    DeviceInfo info;
    info.id = "jack";
    info.name = "JACK Audio Server";
    info.max_input_channels = kJackMaxChannels;
    info.max_output_channels = kJackMaxChannels;
    info.sample_rates = {48000.0};
    info.buffer_sizes = {64, 128, 256, 512, 1024};
    info.is_default_input = true;
    info.is_default_output = true;
    jack_status_t status;
    if (jack_client_t* probe = jack_client_open(
            "pulp_enum", JackNoStartServer, &status)) {
        info.sample_rates = {static_cast<double>(jack_get_sample_rate(probe))};
        int bs = static_cast<int>(jack_get_buffer_size(probe));
        if (bs > 0) info.buffer_sizes = {bs};
        jack_client_close(probe);
    }
    return {info};
}

std::unique_ptr<AudioDevice> JackSystem::create_device(const std::string& device_id) {
    (void)device_id;
    return std::make_unique<JackDevice>("pulp");
}

DeviceInfo JackSystem::default_output_device() {
    auto devs = enumerate_devices();
    return devs.empty() ? DeviceInfo{} : devs.front();
}

DeviceInfo JackSystem::default_input_device() {
    auto devs = enumerate_devices();
    return devs.empty() ? DeviceInfo{} : devs.front();
}

} // namespace pulp::audio::linux_platform

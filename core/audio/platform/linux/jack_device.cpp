#include "jack_device.hpp"
#include <pulp/runtime/log.hpp>

#include <cstring>
#include <algorithm>

namespace pulp::audio::linux_platform {

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

    // Register output ports
    int out_channels = std::min(config_.output_channels, 8);
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
    int in_channels = std::min(config_.input_channels, 8);
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

    runtime::log_info("JACK: connected as '{}' at {} Hz, buffer {} frames, {} out / {} in",
        client_name_, config_.sample_rate, config_.buffer_size,
        out_channels, in_channels);
    return true;
}

void JackDevice::close() {
    if (client_) {
        jack_client_close(client_);
        client_ = nullptr;
    }
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

// ── JackSystem (workstream 02 slice 2.2) ───────────────────────────────

std::vector<DeviceInfo> JackSystem::enumerate_devices() {
    // JACK exposes a single logical "server" as the device. Sample rate
    // and buffer size come from the running server; probe once to read
    // real values rather than hard-coded guesses.
    DeviceInfo info;
    info.id = "jack";
    info.name = "JACK Audio Server";
    info.max_input_channels = 64;
    info.max_output_channels = 64;
    info.default_sample_rate = 48000.0;
    info.buffer_sizes = {64, 128, 256, 512, 1024};
    info.is_default = true;
    jack_status_t status;
    if (jack_client_t* probe = jack_client_open(
            "pulp_enum", JackNoStartServer, &status)) {
        info.default_sample_rate =
            static_cast<double>(jack_get_sample_rate(probe));
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

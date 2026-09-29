#include <pulp/format/detail/null_audio_device.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>

namespace pulp::format::detail {

using audio::BufferView;
using audio::CallbackContext;

namespace {

// A callback this many periods late is treated as a dropped run of periods.
constexpr int kMaxCatchUpPeriods = 4;

DeviceInfo null_device_info(const DeviceConfig* config) {
    DeviceInfo info;
    info.id = kNullAudioDeviceId;
    info.name = "No output (null device)";
    info.max_input_channels = config ? config->input_channels : 2;
    info.max_output_channels = config ? config->output_channels : 2;
    info.sample_rates = {44100.0, 48000.0, 88200.0, 96000.0};
    info.buffer_sizes = {32, 64, 128, 256, 512, 1024};
    if (config) {
        if (std::find(info.sample_rates.begin(), info.sample_rates.end(),
                      config->sample_rate) == info.sample_rates.end())
            info.sample_rates.push_back(config->sample_rate);
        if (std::find(info.buffer_sizes.begin(), info.buffer_sizes.end(),
                      config->buffer_size) == info.buffer_sizes.end())
            info.buffer_sizes.push_back(config->buffer_size);
    }
    info.is_default_input = true;
    info.is_default_output = true;
    return info;
}

} // namespace

NullAudioDevice::~NullAudioDevice() { close(); }

bool NullAudioDevice::open(const DeviceConfig& config) {
    if (config.sample_rate <= 0.0 || config.buffer_size <= 0 ||
        config.input_channels < 0 || config.output_channels < 0)
        return false;
    close();
    config_ = config;
    const auto frames = static_cast<std::size_t>(config.buffer_size);
    input_.assign(static_cast<std::size_t>(config.input_channels), std::vector<float>(frames, 0.0f));
    output_.assign(static_cast<std::size_t>(config.output_channels), std::vector<float>(frames, 0.0f));
    open_ = true;
    return true;
}

void NullAudioDevice::close() {
    stop();
    open_ = false;
}

bool NullAudioDevice::start(AudioCallback callback) {
    if (!open_ || !callback || is_running()) return false;
    callback_ = std::move(callback);
    blocks_.store(0, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { run(); });
    return true;
}

void NullAudioDevice::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
}

DeviceInfo NullAudioDevice::info() const {
    return null_device_info(open_ ? &config_ : nullptr);
}

void NullAudioDevice::run() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(config_.buffer_size / config_.sample_rate));
    const auto frames = static_cast<std::size_t>(config_.buffer_size);

    std::vector<const float*> in_ptrs;
    for (auto& ch : input_) in_ptrs.push_back(ch.data());
    std::vector<float*> out_ptrs;
    for (auto& ch : output_) out_ptrs.push_back(ch.data());

    CallbackContext context;
    context.sample_rate = config_.sample_rate;
    context.buffer_size = config_.buffer_size;

    auto deadline = clock::now();
    while (running_.load(std::memory_order_acquire)) {
        for (auto& ch : output_) std::fill(ch.begin(), ch.end(), 0.0f);
        const BufferView<const float> input(in_ptrs.data(), in_ptrs.size(), frames);
        BufferView<float> output(out_ptrs.data(), out_ptrs.size(), frames);
        callback_(input, output, context);
        context.sample_position += frames;
        blocks_.fetch_add(1, std::memory_order_relaxed);

        deadline += period;
        const auto now = clock::now();
        if (now - deadline > period * kMaxCatchUpPeriods) {
            xruns_.fetch_add(1, std::memory_order_relaxed);
            deadline = now;
        }
        std::this_thread::sleep_until(deadline);
    }
}

std::vector<DeviceInfo> NullAudioSystem::enumerate_devices() {
    return {null_device_info(nullptr)};
}

std::unique_ptr<AudioDevice> NullAudioSystem::create_device(const std::string&) {
    return std::make_unique<NullAudioDevice>();
}

DeviceInfo NullAudioSystem::default_output_device() { return null_device_info(nullptr); }
DeviceInfo NullAudioSystem::default_input_device() { return null_device_info(nullptr); }

std::unique_ptr<AudioSystem> create_null_audio_system() {
    return std::make_unique<NullAudioSystem>();
}

bool null_audio_device_requested(const char* env_value) noexcept {
    if (!env_value) return false;
    const std::string_view v(env_value);
    constexpr std::string_view word = "null";
    if (v.size() != word.size()) return false;
    for (std::size_t i = 0; i < v.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(v[i])) != word[i]) return false;
    return true;
}

} // namespace pulp::format::detail

#include "cmd_audio_plugin_common.hpp"

#include "au_info_plist.hpp"
#include "cli_common.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/events/message_loop_integration.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace pulp::cli::plugin_lab {

std::optional<host::PluginFormat> parse_format(std::string_view value) {
    if (value == "clap" || value == "CLAP") return host::PluginFormat::CLAP;
    if (value == "vst3" || value == "VST3") return host::PluginFormat::VST3;
    if (value == "au" || value == "AU") return host::PluginFormat::AudioUnit;
    if (value == "auv3" || value == "AUv3") return host::PluginFormat::AudioUnitV3;
    if (value == "lv2" || value == "LV2") return host::PluginFormat::LV2;
    return std::nullopt;
}

bool is_apple_audio_unit(host::PluginFormat format) {
    return format == host::PluginFormat::AudioUnit ||
           format == host::PluginFormat::AudioUnitV3;
}

std::uint32_t default_warmup_ms(host::PluginFormat format) {
    return is_apple_audio_unit(format) ? 1000u : 0u;
}

std::uint32_t default_settle_ms(host::PluginFormat format, bool has_initial_params) {
    return is_apple_audio_unit(format) && has_initial_params ? 250u : 0u;
}

host::PluginInfo resolve_plugin_info(const std::string& path,
                                     host::PluginFormat format,
                                     const std::string& unique_id) {
    host::PluginInfo info;
    info.path = path;
    info.format = format;
    info.unique_id = unique_id;
    if (info.unique_id.empty() && is_apple_audio_unit(format))
        info.unique_id = au_info_plist::unique_id_from_bundle(path);

    // AudioComponent enumeration owns AU identity, role, and nominal bus
    // metadata. Resolve it once, in the disposable worker, so inspection and
    // rendering prepare the same descriptor. Enumeration does not instantiate
    // the other components.
    if (is_apple_audio_unit(format) && !info.unique_id.empty()) {
        host::ScanOptions options;
        options.scan_vst3 = false;
        options.scan_clap = false;
        options.scan_lv2 = false;
        const auto installed = host::PluginScanner{}.scan(options);
        const auto found = std::find_if(installed.begin(), installed.end(),
            [&](const host::PluginInfo& candidate) {
                return candidate.format == format &&
                       candidate.unique_id == info.unique_id;
            });
        if (found != installed.end()) {
            const auto requested_path = info.path;
            info = *found;
            if (info.path.empty()) info.path = requested_path;
        }
    }
    return info;
}

void process_discarded_preroll(host::PluginSlot& slot,
                               std::uint32_t duration_ms,
                               std::uint32_t input_channels,
                               std::uint32_t output_channels,
                               std::uint32_t block) {
    if (duration_ms == 0) return;
    audio::Buffer<float> input(input_channels, block), output(output_channels, block);
    input.clear();
    output.clear();
    midi::MidiBuffer midi_in, midi_out;
    state::ParameterEventQueue params;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(duration_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto out = output.view();
        const auto in = static_cast<const audio::Buffer<float>&>(input).view();
        slot.process(out, in, midi_in, midi_out, params, static_cast<int>(block));
        const auto result = events::MessageLoopIntegration::pump_main_loop_for(
            std::chrono::milliseconds(10));
        if (result == events::MainLoopPumpResult::Unsupported ||
            result == events::MainLoopPumpResult::Finished ||
            result == events::MainLoopPumpResult::Stopped)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

platform::ProcessResult run_disposable_worker(const std::vector<std::string>& args,
                                              int timeout_ms) {
    platform::ProcessOptions options;
    options.timeout_ms = timeout_ms;
    options.max_output_bytes = 4u << 20;
    const auto self = current_executable_path();
    if (self.empty()) {
        platform::ProcessResult failed;
        failed.exit_code = -1;
        failed.stderr_output = "cannot resolve the current executable\n";
        return failed;
    }
    return platform::ChildProcess::run(self.string(), args, options);
}

PrivateTempDirectory::PrivateTempDirectory(std::filesystem::path path)
    : path_(std::move(path)) {}

std::optional<PrivateTempDirectory> PrivateTempDirectory::create(std::string_view prefix) {
    try {
        std::random_device random;
        auto path = std::filesystem::temp_directory_path() /
            (std::string(prefix) + "-" + std::to_string(random()) + "-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (ec) return std::nullopt;
        std::filesystem::permissions(
            path, std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace, ec);
        if (ec) {
            std::filesystem::remove_all(path, ec);
            return std::nullopt;
        }
        return PrivateTempDirectory(std::move(path));
    } catch (...) {
        return std::nullopt;
    }
}

PrivateTempDirectory::~PrivateTempDirectory() { cleanup(); }

PrivateTempDirectory::PrivateTempDirectory(PrivateTempDirectory&& other) noexcept
    : path_(std::move(other.path_)) {
    other.path_.clear();
}

PrivateTempDirectory& PrivateTempDirectory::operator=(PrivateTempDirectory&& other) noexcept {
    if (this != &other) {
        cleanup();
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

void PrivateTempDirectory::cleanup() noexcept {
    if (path_.empty()) return;
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
    path_.clear();
}

}  // namespace pulp::cli::plugin_lab

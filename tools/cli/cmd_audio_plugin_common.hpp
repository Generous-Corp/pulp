#pragma once

#include <pulp/host/plugin_slot.hpp>
#include <pulp/platform/child_process.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::cli::plugin_lab {

std::optional<host::PluginFormat> parse_format(std::string_view value);
bool is_apple_audio_unit(host::PluginFormat format);
std::uint32_t default_warmup_ms(host::PluginFormat format);
std::uint32_t default_settle_ms(host::PluginFormat format, bool has_initial_params);

host::PluginInfo resolve_plugin_info(const std::string& path,
                                     host::PluginFormat format,
                                     const std::string& unique_id);

void process_discarded_preroll(host::PluginSlot& slot,
                               std::uint32_t duration_ms,
                               std::uint32_t input_channels,
                               std::uint32_t output_channels,
                               std::uint32_t block);

platform::ProcessResult run_disposable_worker(const std::vector<std::string>& args,
                                              int timeout_ms);

class PrivateTempDirectory {
public:
    static std::optional<PrivateTempDirectory> create(std::string_view prefix);
    ~PrivateTempDirectory();
    PrivateTempDirectory(PrivateTempDirectory&& other) noexcept;
    PrivateTempDirectory& operator=(PrivateTempDirectory&& other) noexcept;
    PrivateTempDirectory(const PrivateTempDirectory&) = delete;
    PrivateTempDirectory& operator=(const PrivateTempDirectory&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    explicit PrivateTempDirectory(std::filesystem::path path);
    void cleanup() noexcept;
    std::filesystem::path path_;
};

}  // namespace pulp::cli::plugin_lab

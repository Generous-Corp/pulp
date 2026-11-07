// Validation and registry glue shared by every system codec backend.

#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/system_audio_codec.hpp>

#include "system_audio_codec_backend.hpp"

#include <pulp/runtime/log.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

namespace pulp::audio {

namespace {

bool valid_bit_depth(const SystemAudioWriteOptions& options) {
    switch (options.encoding) {
    case SystemAudioEncoding::AAC:
        return true;
    case SystemAudioEncoding::ALAC:
        return options.bit_depth == 16 || options.bit_depth == 20 || options.bit_depth == 24 ||
               options.bit_depth == 32;
    case SystemAudioEncoding::LinearPCM:
        if (options.float_pcm)
            return options.container == SystemAudioContainer::CAF;
        return options.bit_depth == 16 || options.bit_depth == 24 || options.bit_depth == 32;
    }
    return false;
}

bool valid_data(const AudioFileData& data) {
    if (data.empty() || data.sample_rate == 0)
        return false;
    const auto frames = data.channels.front().size();
    for (const auto& channel : data.channels)
        if (channel.size() != frames)
            return false;
    return true;
}

// Registry writer: picks the container and encoding from the extension.
class SystemAudioWriter : public FormatWriter {
  public:
    bool write(const std::string& path, const AudioFileData& data) override {
        std::string ext = std::filesystem::path(path).extension().string();
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        SystemAudioWriteOptions options;
        if (!options_for_extension(ext, options))
            return false;
        return write_system_audio_file(path, data, options);
    }

    bool supports_extension(std::string_view ext) const override {
        SystemAudioWriteOptions options;
        return options_for_extension(ext, options);
    }

    std::string format_name() const override {
        return "System codec";
    }

  private:
    static bool options_for_extension(std::string_view ext, SystemAudioWriteOptions& options) {
        if (ext == ".m4a") {
            options.container = SystemAudioContainer::M4A;
            options.encoding = SystemAudioEncoding::AAC;
        } else if (ext == ".aac") {
            options.container = SystemAudioContainer::ADTS;
            options.encoding = SystemAudioEncoding::AAC;
        } else if (ext == ".alac") {
            options.container = SystemAudioContainer::M4A;
            options.encoding = SystemAudioEncoding::ALAC;
            options.bit_depth = 16;
        } else if (ext == ".caf") {
            // Float keeps the whole render, including samples beyond full scale.
            options.container = SystemAudioContainer::CAF;
            options.encoding = SystemAudioEncoding::LinearPCM;
            options.float_pcm = true;
        } else {
            return false;
        }
        return system_audio_can_write(options.container, options.encoding);
    }
};

// A sibling of `target` that no other writer is using, keeping its extension
// so a backend that looks at it sees the same one.
std::filesystem::path temporary_sibling(const std::filesystem::path& target) {
    static std::atomic<std::uint64_t> counter{0};
    const auto stamp =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    auto name = "." + target.stem().string() + ".pulp-partial-" + std::to_string(stamp) + "-" +
                std::to_string(counter.fetch_add(1)) + target.extension().string();
    return target.parent_path() / name;
}

} // namespace

bool system_audio_can_write(SystemAudioContainer container, SystemAudioEncoding encoding) {
    return detail::system_backend_can_write(container, encoding);
}

bool write_system_audio_file(const std::string& path, const AudioFileData& data,
                             const SystemAudioWriteOptions& options) {
    if (path.empty() || !valid_data(data) || !valid_bit_depth(options) ||
        !system_audio_can_write(options.container, options.encoding))
        return false;

    // Encode beside the target and rename over it only on success, so a codec
    // refusal or a full disk leaves any existing file at `path` untouched.
    const std::filesystem::path target(path);
    const auto temporary = temporary_sibling(target);
    std::error_code ec;
    if (!detail::system_backend_write(temporary.string(), data, options)) {
        std::filesystem::remove(temporary, ec);
        return false;
    }
    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        runtime::log_warn("System codec: could not replace {} ({})", path, ec.message());
        std::filesystem::remove(temporary, ec);
        return false;
    }
    return true;
}

std::unique_ptr<FormatWriter> create_system_audio_writer() {
    // A platform with no system codec registers nothing.
    if (!system_audio_can_write(SystemAudioContainer::M4A, SystemAudioEncoding::AAC))
        return nullptr;
    return std::make_unique<SystemAudioWriter>();
}

} // namespace pulp::audio

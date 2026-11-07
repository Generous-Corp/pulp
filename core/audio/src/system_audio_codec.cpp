// Validation and registry glue shared by every system codec backend.

#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/system_audio_codec.hpp>

#include "system_audio_codec_backend.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <string_view>

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
        const auto dot = path.find_last_of('.');
        if (dot == std::string::npos)
            return false;
        std::string ext = path.substr(dot);
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

} // namespace

bool system_audio_can_write(SystemAudioContainer container, SystemAudioEncoding encoding) {
    return detail::system_backend_can_write(container, encoding);
}

bool write_system_audio_file(const std::string& path, const AudioFileData& data,
                             const SystemAudioWriteOptions& options) {
    if (path.empty() || !valid_data(data) || !valid_bit_depth(options) ||
        !system_audio_can_write(options.container, options.encoding))
        return false;
    return detail::system_backend_write(path, data, options);
}

std::unique_ptr<FormatWriter> create_system_audio_writer() {
    // A platform with no system codec registers nothing.
    if (!system_audio_can_write(SystemAudioContainer::M4A, SystemAudioEncoding::AAC))
        return nullptr;
    return std::make_unique<SystemAudioWriter>();
}

} // namespace pulp::audio

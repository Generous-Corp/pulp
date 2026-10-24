// libFuzzer entry point over every reader the FormatRegistry registers.
//
// The input is written to a scratch file once per supported extension and
// handed to the registry, so one corpus exercises FLAC, MP3, OGG, AIFF, WAV and
// whatever platform readers are present. Output ceilings are kept small: the
// property under test is "no crash, no runaway allocation", and a legitimate
// multi-gigabyte decode would only trip the sanitizer's own allocation limit.

#include <pulp/audio/format_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

struct ScratchDir {
    fs::path path;

    ScratchDir() {
#ifdef _WIN32
        const auto pid = static_cast<long long>(_getpid());
#else
        const auto pid = static_cast<long long>(getpid());
#endif
        path = fs::temp_directory_path() / ("pulp-format-registry-fuzz-" + std::to_string(pid));
        std::error_code ec;
        fs::create_directories(path, ec);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

const ScratchDir& scratch_dir() {
    static const ScratchDir dir;
    return dir;
}

const std::vector<std::string>& read_extensions() {
    static const std::vector<std::string> extensions =
        pulp::audio::FormatRegistry::instance().supported_read_extensions();
    return extensions;
}

bool write_input(const fs::path& path, const uint8_t* data, size_t size) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good())
        return false;
    if (size > 0)
        output.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return output.good();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    pulp::audio::DecodeLimits limits;
    limits.max_frames = 1u << 22;
    limits.max_channels = 16u;
    limits.max_output_bytes = 64u * 1024u * 1024u;

    auto& registry = pulp::audio::FormatRegistry::instance();
    for (const auto& extension : read_extensions()) {
        const auto path = scratch_dir().path / ("input" + extension);
        if (!write_input(path, data, size))
            continue;
        (void)registry.read_info(path.string());
        (void)registry.read(path.string(), limits);
    }
    return 0;
}

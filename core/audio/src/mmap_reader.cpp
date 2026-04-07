#include <pulp/audio/mmap_reader.hpp>
#include <pulp/audio/format_registry.hpp>

namespace pulp::audio {

bool MemoryMappedAudioReader::open(std::string_view path) {
    close();
    path_ = std::string(path);

    if (!mmap_.open(path))
        return false;

    // Get file info via the format registry
    auto file_info = FormatRegistry::instance().read_info(path_);
    if (!file_info) {
        close();
        return false;
    }

    info_ = *file_info;
    return true;
}

void MemoryMappedAudioReader::close() {
    mmap_.close();
    info_ = {};
    path_.clear();
}

bool MemoryMappedAudioReader::read_frames(float** dest_channels, uint32_t num_channels,
                                           uint64_t start_frame, uint64_t num_frames) {
    if (!is_open()) return false;

    // For now, read the full file and extract the range
    // A more optimized version would decode only the requested range
    auto data = FormatRegistry::instance().read(path_);
    if (!data) return false;

    uint32_t ch_count = std::min(num_channels, data->num_channels());
    uint64_t avail = data->num_frames();
    uint64_t start = std::min(start_frame, avail);
    uint64_t count = std::min(num_frames, avail - start);

    for (uint32_t c = 0; c < ch_count; ++c)
        for (uint64_t f = 0; f < count; ++f)
            dest_channels[c][f] = data->channels[c][static_cast<size_t>(start + f)];

    return true;
}

std::optional<AudioFileData> MemoryMappedAudioReader::read_all() {
    if (!is_open()) return std::nullopt;
    return FormatRegistry::instance().read(path_);
}

}  // namespace pulp::audio

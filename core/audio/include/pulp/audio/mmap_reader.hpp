#pragma once

// MemoryMappedAudioReader — zero-copy audio file access via memory mapping.
// Combines MemoryMappedFile with codec decoding for efficient large-file access.

#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>
#include <string>
#include <string_view>
#include <optional>

namespace pulp::audio {

/// Memory-mapped audio file reader.
/// Maps the file into memory and decodes on demand.
/// Ideal for large sample libraries where loading the entire file is impractical.
class MemoryMappedAudioReader {
public:
    MemoryMappedAudioReader() = default;

    /// Open and memory-map an audio file. Returns false on failure.
    bool open(std::string_view path);

    /// Close the mapped file.
    void close();

    /// Whether a file is currently mapped.
    bool is_open() const { return mmap_.is_open(); }

    /// File info (available after open)
    const AudioFileInfo& info() const { return info_; }

    /// Read a range of frames into deinterleaved float buffers.
    /// Decodes from the mapped data on demand.
    bool read_frames(float** dest_channels, uint32_t num_channels,
                     uint64_t start_frame, uint64_t num_frames);

    /// Read the entire file (convenience — same as read_audio_file but from mapped data)
    std::optional<AudioFileData> read_all();

    /// Raw mapped bytes (for custom decoding)
    const uint8_t* data() const { return mmap_.data(); }
    size_t size() const { return mmap_.size(); }

private:
    runtime::MemoryMappedFile mmap_;
    AudioFileInfo info_;
    std::string path_;
};

}  // namespace pulp::audio

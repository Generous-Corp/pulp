#pragma once

// MemoryMappedAudioReader — zero-copy audio file access via memory mapping.
// Combines MemoryMappedFile with codec decoding for efficient large-file access.

#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>
#include <memory>
#include <string>
#include <string_view>
#include <optional>

namespace pulp::audio {

/// Memory-mapped audio file reader.
/// Maps the file into memory and decodes on demand.
/// Ideal for large sample libraries where loading the entire file is impractical.
class MemoryMappedAudioReader {
public:
    // All special members are out-of-line (defined in the .cpp where RangedState
    // is complete) — the pimpl unique_ptr requires a complete type for
    // construction-cleanup, destruction, and move.
    MemoryMappedAudioReader();
    ~MemoryMappedAudioReader();
    MemoryMappedAudioReader(MemoryMappedAudioReader&&) noexcept;
    MemoryMappedAudioReader& operator=(MemoryMappedAudioReader&&) noexcept;

    /// Open and memory-map an audio file. Returns false on failure.
    bool open(std::string_view path);

    /// Close the mapped file.
    void close();

    /// Whether a file is currently mapped.
    bool is_open() const { return mmap_.is_open(); }

    /// File info (available after open)
    const AudioFileInfo& info() const { return info_; }

    /// Read a range of frames into deinterleaved float buffers. Seekable formats
    /// use the mapped ranged decoder; unsupported formats or an unexpected
    /// ranged decode failure fall back to a cached whole-file decode.
    /// Returns false on error. Frames past end-of-file are zero-filled. RT note:
    /// this performs decode/copy work, so it is for the control or background
    /// reader thread, never the audio callback.
    /// Not thread-safe: the persistent ranged reader carries a single seek
    /// position plus lazily-initialized scratch/fallback state, so concurrent
    /// calls on one instance race. Serialize calls (one streaming reader thread
    /// per reader), or use one MemoryMappedAudioReader per thread.
    bool read_frames(float** dest_channels, uint32_t num_channels,
                     uint64_t start_frame, uint64_t num_frames);

    /// Strict counterpart to read_frames(). Returns false unless the requested
    /// frames were served by the mapped ranged decoder; it never starts or uses
    /// the whole-file fallback cache.
    bool read_frames_ranged_only(float** dest_channels, uint32_t num_channels,
                                 uint64_t start_frame, uint64_t num_frames);

    /// True when a ranged (seek-based, no whole-file decode) reader is active
    /// for this file. False means read_frames falls back to a one-time
    /// whole-file decode cached on first use (e.g. a format the streaming
    /// reader can't seek).
    bool supports_ranged_read() const;

    /// Read the entire file (convenience — same as read_audio_file but from mapped data)
    std::optional<AudioFileData> read_all();

    /// Raw mapped bytes (for custom decoding)
    const uint8_t* data() const { return mmap_.data(); }
    size_t size() const { return mmap_.size(); }

private:
    // Ranged (seek-based) decoder state over the mapped bytes; nullptr when the
    // active format can't be range-read (read_frames then decode-once-caches).
    // Declared AFTER mmap_ so it is destroyed BEFORE the mapping it references.
    struct RangedState;

    runtime::MemoryMappedFile mmap_;
    AudioFileInfo info_;
    std::string path_;
    std::unique_ptr<RangedState> ranged_;

    bool read_frames_impl(float** dest_channels, uint32_t num_channels,
                          uint64_t start_frame, uint64_t num_frames,
                          bool allow_whole_file_fallback);
};

}  // namespace pulp::audio

#pragma once

// MemoryMappedAudioReader — zero-copy audio file access via memory mapping.
// Combines MemoryMappedFile with codec decoding for efficient large-file access.

#include <limits>
#include <memory>
#include <optional>
#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/memory_mapped_file.hpp>
#include <string>
#include <string_view>

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
    bool open(std::string_view path, std::size_t maximum_mapped_bytes);

    /// Close the mapped file.
    void close();

    /// Whether an immutable snapshot of the source is currently mapped.
    bool is_open() const;

    /// File info (available after open)
    const AudioFileInfo& info() const {
        return info_;
    }

    /// Read a range of frames into deinterleaved float buffers. Seekable formats
    /// use the mapped ranged decoder; unsupported formats or an unexpected
    /// ranged decode failure fall back to a cached whole-file decode.
    /// Returns false on error. Frames past end-of-file are zero-filled. RT note:
    /// this performs decode/copy work, so it is for the control or background
    /// reader thread, never the audio callback.
    /// Concurrent calls on one instance are serialized internally. Open, close,
    /// move, and destruction must not overlap reads. This API remains non-RT:
    /// callers may block on decoding, allocation, I/O, or another reader.
    bool read_frames(float** dest_channels, uint32_t num_channels, uint64_t start_frame,
                     uint64_t num_frames);

    /// Strict counterpart to read_frames(). Returns false unless the requested
    /// frames were served by the mapped ranged decoder; it never starts or uses
    /// the whole-file fallback cache.
    bool read_frames_ranged_only(float** dest_channels, uint32_t num_channels, uint64_t start_frame,
                                 uint64_t num_frames);

    /// True when a ranged (seek-based, no whole-file decode) reader is active
    /// for this file. False means read_frames falls back to a one-time
    /// whole-file decode cached on first use (e.g. a format the streaming
    /// reader can't seek).
    bool supports_ranged_read() const;

    /// Read the entire immutable source snapshot.
    std::optional<AudioFileData> read_all();

    /// Raw bytes from the immutable source snapshot (for custom decoding).
    const uint8_t* data() const;
    size_t size() const;

    /// Copy the access policy captured by the retained source handle to an
    /// existing artifact. Control thread only.
    bool copy_access_policy_to(std::string_view destination) const noexcept;
    runtime::AccessPolicyTarget
    prepare_access_policy_target(std::string_view destination) const noexcept;
    bool path_refers_to_open_file(std::string_view path) const noexcept;
    runtime::FileIdentity opened_file_identity() const noexcept;

  private:
    // Ranged (seek-based) decoder state over the mapped bytes; nullptr when the
    // active format can't be range-read (read_frames then decode-once-caches).
    // BackingState retains the original source handle for identity/access-policy
    // operations and owns an immutable temporary snapshot used for all reads.
    struct BackingState;
    struct RangedState;

    std::unique_ptr<BackingState> backing_;
    AudioFileInfo info_;
    std::string path_;
    std::unique_ptr<RangedState> ranged_;

    bool read_frames_impl(float** dest_channels, uint32_t num_channels, uint64_t start_frame,
                          uint64_t num_frames, bool allow_whole_file_fallback);
};

} // namespace pulp::audio

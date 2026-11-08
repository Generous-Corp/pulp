#pragma once

/// @file audio_reader.hpp
/// Seekable audio reader over a file, a block of memory, or any byte source.
///
/// AudioReader decodes only the frames a caller asks for, and its memory does
/// not grow with the file. The time a read takes depends on the format:
///
/// - WAV and AIFF compute the byte offset of any frame directly.
/// - FLAC and Ogg Vorbis seek within the encoded stream, so reading a window
///   near the end of a three-hour file costs about the same as near the start.
/// - MP3 has no frame index. Opening a file without a Xing or Info header
///   reads every frame header once to count the frames, and a seek backward
///   decodes forward from the start of the stream, so its cost grows with the
///   target offset. Reads that continue where the last one ended never seek.
///
/// A format without a seekable decoder (for example AAC on Apple platforms,
/// which only opens from a path) is decoded once on open and served from that
/// copy, as is a FLAC or Ogg stream whose header gives no length.
/// supports_ranged_read() reports which of the two a reader uses, and
/// AudioReaderFallback::Refuse opens only readers that seek.
///
/// An empty stream (zero frames) does not open.
///
/// Threading: one AudioReader serves one caller at a time. Opening and reading
/// decode, allocate and perform I/O, so they belong on a control or background
/// thread, never the audio callback.

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/format_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::audio {

/// Random-access source of encoded bytes. Implement this to decode from an
/// archive member, a network cache or anything else that can seek.
class AudioByteSource {
  public:
    virtual ~AudioByteSource() = default;

    /// Copies up to `bytes` bytes at the current position into `destination`
    /// and advances by the count returned. Zero means end of data or an error.
    virtual std::size_t read(void* destination, std::size_t bytes) = 0;

    /// Moves to the absolute byte offset `position`. Returns false when the
    /// offset is past the end or the source cannot seek.
    virtual bool seek(std::uint64_t position) = 0;

    /// Current absolute byte offset.
    virtual std::uint64_t position() const = 0;

    /// Total size in bytes.
    virtual std::uint64_t size() const = 0;
};

/// A byte source over memory the caller keeps alive for the source's lifetime.
std::unique_ptr<AudioByteSource> make_memory_byte_source(std::span<const std::uint8_t> bytes);

/// A byte source over a file opened for reading, or nullptr if it cannot open.
std::unique_ptr<AudioByteSource> make_file_byte_source(const std::string& path);

/// What an open does with a stream it cannot seek by frame: a format with no
/// seekable decoder, or a FLAC or Ogg stream whose header gives no length.
enum class AudioReaderFallback {
    DecodeOnce, ///< Decode the whole stream on open, within the limits.
    Refuse,     ///< Return nullptr without decoding.
};

/// Decodes ranges of frames from one encoded audio stream.
///
/// Decode limits: opening refuses a stream whose header claims more frames or
/// channels than `limits` allows. Because a ranged reader never holds the
/// whole stream, `max_output_bytes` bounds each allocating read() instead of
/// the file's length; a stream decoded once on open is held to every ceiling.
class AudioReader {
  public:
    /// Opens `path`. The format comes from the extension, then from the first
    /// bytes when the extension is unknown. Any format the registry reads is
    /// accepted; formats without a seekable decoder are decoded once here.
    static std::unique_ptr<AudioReader>
    open_file(const std::string& path, const DecodeLimits& limits = {},
              AudioReaderFallback fallback = AudioReaderFallback::DecodeOnce);

    /// Opens encoded bytes held in memory without copying them. `bytes` must
    /// outlive the reader. `format_hint` is an extension such as ".flac"; when
    /// it is empty or unknown the format is detected from the first bytes.
    /// Only seekable formats open from memory.
    static std::unique_ptr<AudioReader>
    open_memory(std::span<const std::uint8_t> bytes, std::string_view format_hint = {},
                const DecodeLimits& limits = {},
                AudioReaderFallback fallback = AudioReaderFallback::DecodeOnce);

    /// Refused at compile time: the reader keeps a view of `bytes`, so a
    /// temporary vector would be destroyed while the reader still reads it.
    /// Keep the vector alive in a variable, or use open_source() with a byte
    /// source that owns its data.
    static std::unique_ptr<AudioReader>
    open_memory(std::vector<std::uint8_t>&& bytes, std::string_view format_hint = {},
                const DecodeLimits& limits = {},
                AudioReaderFallback fallback = AudioReaderFallback::DecodeOnce) = delete;

    /// Opens a caller-supplied byte source, which the reader then owns. Same
    /// format rules as open_memory(). Ogg Vorbis needs its whole stream in
    /// memory to seek, so a byte source holding Ogg is copied into memory
    /// once. That copy is refused above the limits' max_output_bytes and above
    /// 2 GiB, the most stb_vorbis addresses; with default limits an Ogg byte
    /// source can therefore allocate up to 2 GiB of encoded bytes.
    static std::unique_ptr<AudioReader>
    open_source(std::unique_ptr<AudioByteSource> source, std::string_view format_hint = {},
                const DecodeLimits& limits = {},
                AudioReaderFallback fallback = AudioReaderFallback::DecodeOnce);

    ~AudioReader();
    AudioReader(const AudioReader&) = delete;
    AudioReader& operator=(const AudioReader&) = delete;

    /// Sample rate, channel count, frame count and format name.
    const AudioFileInfo& info() const;

    /// True when reads seek inside the encoded stream; false when they are
    /// served from a copy decoded on open.
    bool supports_ranged_read() const;

    /// Decodes `frames` frames starting at `frame_offset` into the planar
    /// buffers `destinations[0..channels)`. Channels beyond the file's count
    /// and frames past the end are zero-filled, and fewer channels than the
    /// file has reads the first `channels` of them. Returns the number of
    /// frames decoded from the stream, which is less than `frames` at the end
    /// of the stream or after a decode error.
    std::uint64_t read(std::uint64_t frame_offset, float* const* destinations,
                       std::uint32_t channels, std::uint64_t frames);

    /// Decodes up to `frames` frames from `frame_offset` into new storage,
    /// with every channel of the file. Refuses (returns nullopt) a request
    /// whose output exceeds the limits the reader was opened with.
    std::optional<AudioFileData> read(std::uint64_t frame_offset, std::uint64_t frames);

    struct Decoder;

  private:
    AudioReader(std::unique_ptr<Decoder> decoder, const DecodeLimits& limits);

    std::unique_ptr<Decoder> decoder_;
    DecodeLimits limits_;
};

} // namespace pulp::audio

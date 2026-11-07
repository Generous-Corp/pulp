#pragma once

// Audio format codec registry — register readers/writers by extension/MIME type.
// Provides unified audio file I/O across all supported formats.

#include <pulp/audio/audio_file.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>

namespace pulp::audio {

/// Resource ceilings a reader enforces on decoded output. Container headers
/// are untrusted: a reader checks the claimed size against these limits before
/// allocating, then keeps checking as frames arrive, so a tiny file cannot
/// request terabytes of output. A zero field means that ceiling is not
/// enforced. The plain read(path) overloads apply these defaults, which admit
/// ordinary long recordings (2^30 frames is over three hours of stereo at
/// 96 kHz within 4 GiB of float output); callers reading longer material pass
/// larger limits explicitly, and trusted local-file callers whose path was
/// chosen by the user or host pass unbounded(). Field names match
/// WavDecodeLimits.
struct DecodeLimits {
    std::uint64_t max_frames = 1ull << 30;
    std::uint32_t max_channels = 64u;
    std::uint64_t max_output_bytes = 4ull * 1024u * 1024u * 1024u;

    /// No ceilings at all: every field zero.
    static DecodeLimits unbounded() {
        DecodeLimits limits;
        limits.max_frames = 0u;
        limits.max_channels = 0u;
        limits.max_output_bytes = 0u;
        return limits;
    }
};

/// One encoder setting a writer applies, with the value write() uses.
struct FormatQualityOption {
    std::string name;          ///< Stable identifier, e.g. "compression_level".
    std::string description;   ///< What the setting controls.
    std::string default_value; ///< The value write() applies.
};

/// What a reader accepts or a writer produces, for choosing a format before
/// reading or writing. Empty lists mean "not restricted by this format", not
/// "unknown"; a custom reader or writer that does not override capabilities()
/// reports only its name.
struct FormatCapabilities {
    std::string format_name;
    /// Lower-case extensions with the leading dot, e.g. ".flac".
    std::vector<std::string> extensions;
    /// Integer PCM bit depths. Empty for lossy formats, which store no depth.
    std::vector<std::uint32_t> bit_depths;
    /// True when 32-bit float samples are read or written.
    bool float_samples = false;
    /// Sample rates the format allows. Empty means any positive rate.
    std::vector<std::uint32_t> sample_rates;
    /// Most channels the format allows. Zero means no format limit.
    std::uint32_t max_channels = 0;
    /// True when the stored stream is compressed.
    bool compressed = false;
    /// False when encoding discards information.
    bool lossless = true;
    /// Encoder settings write() applies (writers only). None is selectable
    /// through FormatWriter::write; each is reported at the value it uses.
    std::vector<FormatQualityOption> quality_options;
};

/// Audio format reader interface
class FormatReader {
public:
    virtual ~FormatReader() = default;

    /// Read file info (metadata only)
    virtual std::optional<AudioFileInfo> read_info(const std::string& path) = 0;

    /// Read entire file into memory
    virtual std::optional<AudioFileData> read(const std::string& path) = 0;

    /// Read entire file into memory, refusing output beyond `limits`. Every
    /// built-in reader overrides this; the default exists for external readers
    /// that predate limits and falls back to read(path).
    virtual std::optional<AudioFileData> read(const std::string& path, const DecodeLimits& limits) {
        (void)limits;
        return read(path);
    }

    /// Whether this reader supports the given file extension (e.g., ".flac")
    virtual bool supports_extension(std::string_view ext) const = 0;

    /// Format name (e.g., "FLAC", "MP3")
    virtual std::string format_name() const = 0;

    /// What this reader decodes. Built-in readers report their full support;
    /// the default reports only format_name().
    virtual FormatCapabilities capabilities() const {
        FormatCapabilities caps;
        caps.format_name = format_name();
        return caps;
    }
};

/// Audio format writer interface
class FormatWriter {
public:
    virtual ~FormatWriter() = default;

    /// Write audio data to a file
    virtual bool write(const std::string& path, const AudioFileData& data) = 0;

    /// Whether this writer supports the given file extension
    virtual bool supports_extension(std::string_view ext) const = 0;

    /// Format name
    virtual std::string format_name() const = 0;

    /// What write() produces. Built-in writers report their full support;
    /// the default reports only format_name().
    virtual FormatCapabilities capabilities() const {
        FormatCapabilities caps;
        caps.format_name = format_name();
        return caps;
    }
};

/// Registry of audio format readers and writers
class FormatRegistry {
public:
    /// Get the global singleton registry (pre-populated with built-in formats)
    static FormatRegistry& instance();

    /// Register a custom reader
    void register_reader(std::unique_ptr<FormatReader> reader);

    /// Register a custom writer
    void register_writer(std::unique_ptr<FormatWriter> writer);

    /// Find a reader for the given file extension
    FormatReader* find_reader(std::string_view extension) const;

    /// Find a writer for the given file extension
    FormatWriter* find_writer(std::string_view extension) const;

    /// Read any supported audio file (dispatches to the right reader)
    std::optional<AudioFileInfo> read_info(const std::string& path) const;
    std::optional<AudioFileData> read(const std::string& path) const;

    /// Read with explicit decode ceilings. Every built-in reader refuses a
    /// file whose header or decoded output would exceed them; a custom reader
    /// registered without a limits overload reads unbounded.
    std::optional<AudioFileData> read(const std::string& path, const DecodeLimits& limits) const;

    /// Write audio data (dispatches to the right writer based on extension)
    bool write(const std::string& path, const AudioFileData& data) const;

    /// List all supported read extensions
    std::vector<std::string> supported_read_extensions() const;

    /// List all supported write extensions
    std::vector<std::string> supported_write_extensions() const;

    /// Capabilities of every registered reader, in registration order. A
    /// format registered twice (for example a platform reader that also
    /// opens WAV) appears once per reader.
    std::vector<FormatCapabilities> reader_capabilities() const;

    /// Capabilities of every registered writer, in registration order.
    std::vector<FormatCapabilities> writer_capabilities() const;

  private:
    FormatRegistry();
    std::vector<std::unique_ptr<FormatReader>> readers_;
    std::vector<std::unique_ptr<FormatWriter>> writers_;
};

}  // namespace pulp::audio

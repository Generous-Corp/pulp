#pragma once

// Audio format codec registry — register readers/writers by extension/MIME type.
// Provides unified audio file I/O across all supported formats.

#include <pulp/audio/audio_file.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <memory>

namespace pulp::audio {

/// Audio format reader interface
class FormatReader {
public:
    virtual ~FormatReader() = default;

    /// Read file info (metadata only)
    virtual std::optional<AudioFileInfo> read_info(const std::string& path) = 0;

    /// Read entire file into memory
    virtual std::optional<AudioFileData> read(const std::string& path) = 0;

    /// Whether this reader supports the given file extension (e.g., ".flac")
    virtual bool supports_extension(std::string_view ext) const = 0;

    /// Format name (e.g., "FLAC", "MP3")
    virtual std::string format_name() const = 0;
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

    /// Write audio data (dispatches to the right writer based on extension)
    bool write(const std::string& path, const AudioFileData& data) const;

    /// List all supported read extensions
    std::vector<std::string> supported_read_extensions() const;

    /// List all supported write extensions
    std::vector<std::string> supported_write_extensions() const;

private:
    FormatRegistry();
    std::vector<std::unique_ptr<FormatReader>> readers_;
    std::vector<std::unique_ptr<FormatWriter>> writers_;
};

}  // namespace pulp::audio

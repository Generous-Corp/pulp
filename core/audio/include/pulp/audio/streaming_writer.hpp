#pragma once

// StreamingAudioFormatWriter — chunked write interface for audio files.
// Allows writing audio in blocks without holding the entire file in memory.

#include <pulp/audio/audio_file.hpp>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <fstream>
#include <functional>

namespace pulp::audio {

/// Streaming audio writer — writes audio in chunks to a file.
/// Currently supports WAV format. Call open(), write_frames() N times, then close().
class StreamingWriter {
public:
    StreamingWriter() = default;
    ~StreamingWriter();

    /// Open a file for streaming write.
    /// sample_rate and num_channels must be known up front.
    bool open(std::string_view path, uint32_t sample_rate, uint32_t num_channels,
              uint32_t bits_per_sample = 16);

    /// Write interleaved frames. Returns number of frames written.
    int write_frames(const float* interleaved_data, int num_frames);

    /// Write deinterleaved channel data.
    int write_frames(const float* const* channels, int num_channels, int num_frames);

    /// Close the file (finalizes the WAV header with correct sizes).
    void close();

    /// Whether the writer is currently open.
    bool is_open() const { return file_.is_open(); }

    /// Total frames written so far.
    uint64_t frames_written() const { return frames_written_; }

    // No copy
    StreamingWriter(const StreamingWriter&) = delete;
    StreamingWriter& operator=(const StreamingWriter&) = delete;

private:
    std::ofstream file_;
    uint32_t sample_rate_ = 0;
    uint32_t num_channels_ = 0;
    uint32_t bits_per_sample_ = 16;
    uint64_t frames_written_ = 0;
    size_t data_start_pos_ = 0;
};

}  // namespace pulp::audio

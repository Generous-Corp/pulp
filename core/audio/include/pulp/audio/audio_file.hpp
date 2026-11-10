#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>

namespace pulp::audio {

// Audio file metadata
struct AudioFileInfo {
    uint32_t sample_rate = 0;
    uint32_t num_channels = 0;
    uint64_t num_frames = 0;
    uint32_t bits_per_sample = 0;
    std::string format;  // "WAV", "FLAC", "OGG", "MP3"
    double duration_seconds = 0;
};

// Read an audio file into float buffers (uses CHOC internally)
// Returns deinterleaved float channels
struct AudioFileData {
    std::vector<std::vector<float>> channels;
    uint32_t sample_rate = 0;

    uint32_t num_channels() const { return static_cast<uint32_t>(channels.size()); }
    uint64_t num_frames() const { return channels.empty() ? 0 : channels[0].size(); }
    bool empty() const { return channels.empty() || channels[0].empty(); }
};

// Read audio file info without loading data
std::optional<AudioFileInfo> read_audio_file_info(const std::string& path);

// Read entire audio file into memory
std::optional<AudioFileData> read_audio_file(const std::string& path);

// Read an audio file through one open: `admit` sees the header the decoder is
// about to size its output from, and a false return refuses the file before
// anything is allocated. A separate info read followed by a decode would
// check one open's header and allocate from another's.
std::optional<AudioFileData>
read_audio_file(const std::string& path, const std::function<bool(const AudioFileInfo&)>& admit);

// WAV sample format for writing. Int16 is the compact default (≈ −96 dBFS
// noise floor); Int24 is a middle ground (≈ −144 dBFS floor, integer, universal
// DAW/tool compatibility, 75% the size of float); Float32 preserves the full
// render — needed when the downstream analysis cares about residuals below the
// int24 floor or about samples beyond ±1.0 (no hard clip on write).
enum class WavBitDepth { Int16, Int24, Float32 };

// Write audio data to a WAV file (int16). Published atomically, see below.
bool write_wav_file(const std::string& path, const AudioFileData& data);

// Write audio data to a WAV file at the given sample format. The file is
// published atomically and replaces an existing file at `path`.
bool write_wav_file(const std::string& path, const AudioFileData& data,
                    WavBitDepth bit_depth);

// Whether a write may replace a file that already exists at the target.
enum class WavReplace { Replace, FailIfExists };

// Write a WAV file and publish it atomically: the bytes go to a temporary file
// in the target's directory, are flushed to disk, and only then renamed onto
// `path`. The final path therefore either holds the complete new file or is
// untouched (a previous file at `path` survives a failed write); a reader never
// sees a truncated file, and no temporary file is left behind on failure.
// With FailIfExists an existing file at `path` makes the call return false and
// leaves it as it was.
bool write_wav_file(const std::string& path, const AudioFileData& data, WavBitDepth bit_depth,
                    WavReplace replace);

// The publication step on its own, for writers that are not WAV: `produce`
// writes the whole file to the stream it is given and returns false to abandon
// the write (an error or a cancel). Nothing appears at `path` unless it returns
// true and the flush and rename succeed.
bool write_file_atomically(const std::string& path, WavReplace replace,
                           const std::function<bool(std::ostream&)>& produce);

// Write audio data to an already-open seekable stream. The caller retains
// ownership and is responsible for durably syncing/closing its backing file.
bool write_wav_stream(std::ostream& output, const AudioFileData& data, WavBitDepth bit_depth);

// ── Sample format conversion ─────────────────────────────────────────────

// Convert int16 samples to float (-1.0 to 1.0)
void int16_to_float(const int16_t* src, float* dst, size_t count);

// Convert int24 (packed 3 bytes) to float
void int24_to_float(const uint8_t* src, float* dst, size_t count);

// Convert int32 samples to float
void int32_to_float(const int32_t* src, float* dst, size_t count);

// Convert float to int16
void float_to_int16(const float* src, int16_t* dst, size_t count);

// Convert float to int32
void float_to_int32(const float* src, int32_t* dst, size_t count);

} // namespace pulp::audio

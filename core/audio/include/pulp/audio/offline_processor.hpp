#pragma once

// Offline audio processing — render an entire file through a processor chain.
// Useful for batch processing, bouncing, and golden-file test generation.

#include <pulp/audio/audio_file.hpp>
#include <functional>
#include <string>
#include <optional>

namespace pulp::audio {

/// Callback for processing a block of audio.
/// Receives interleaved input, writes interleaved output.
/// Both buffers have `channels * block_size` elements.
using OfflineProcessCallback = std::function<void(
    const float* input, float* output,
    int channels, int block_size, double sample_rate
)>;

/// Process an entire audio file through a callback function.
/// Returns the processed audio, or nullopt on failure.
std::optional<AudioFileData> offline_process(
    const AudioFileData& input,
    OfflineProcessCallback process_fn,
    int block_size = 512
);

/// Process a file on disk, writing the result to another file.
/// Format is determined by the output file extension.
bool offline_process_file(
    const std::string& input_path,
    const std::string& output_path,
    OfflineProcessCallback process_fn,
    int block_size = 512
);

/// Simple gain processing for testing
AudioFileData apply_gain(const AudioFileData& input, float gain_linear);

}  // namespace pulp::audio

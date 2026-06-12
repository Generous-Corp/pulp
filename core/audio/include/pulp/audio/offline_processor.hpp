#pragma once

// Offline audio processing — render an entire file through a processor chain.
// Useful for batch processing, bouncing, and golden-file test generation.

#include <pulp/audio/audio_file.hpp>
#include <functional>
#include <string>
#include <optional>
#include <vector>
#include <cstdint>

namespace pulp::audio {

/// Callback for processing a block of audio.
/// Receives interleaved input, writes interleaved output.
/// Both buffers have `channels * block_size` elements.
using OfflineProcessCallback = std::function<void(
    const float* input, float* output,
    int channels, int block_size, double sample_rate
)>;

/// Per-block metadata for deterministic offline renders.
struct OfflineRenderBlockContext {
    uint64_t block_index = 0;
    uint64_t sample_position = 0;
    int frames = 0;
    int scheduled_block_size = 0;
    double sample_rate = 0.0;
    double time_seconds = 0.0;
    double position_beats = 0.0;
    double tempo_bpm = 120.0;
    double render_speed_ratio = 1.0;
    uint64_t state_generation = 0;
    uint64_t deterministic_seed = 0;
};

enum class OfflineRenderTailPolicy : uint8_t {
    Truncate = 0,
    RenderTail,
};

/// Advanced offline render options. `block_size_schedule` is consumed in order;
/// when the render is longer than the schedule, the final scheduled size
/// repeats. Empty schedule falls back to `fallback_block_size`.
struct OfflineRenderOptions {
    int fallback_block_size = 512;
    std::vector<int> block_size_schedule;
    uint64_t start_sample_position = 0;
    double start_position_beats = 0.0;
    double tempo_bpm = 120.0;
    double render_speed_ratio = 1.0;
    uint64_t state_generation = 0;
    uint64_t deterministic_seed = 0;
    OfflineRenderTailPolicy tail_policy = OfflineRenderTailPolicy::Truncate;
    uint64_t tail_frames = 0;
};

/// Callback for deterministic offline render blocks.
using OfflineRenderCallback = std::function<void(
    const float* input, float* output,
    int channels, const OfflineRenderBlockContext& context
)>;

/// Render an entire in-memory audio file with deterministic block metadata.
std::optional<AudioFileData> offline_render(
    const AudioFileData& input,
    OfflineRenderCallback render_fn,
    const OfflineRenderOptions& options = {}
);

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

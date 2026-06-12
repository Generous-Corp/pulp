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

struct OfflineRenderStem {
    std::string name;
    uint32_t first_channel = 0;
    uint32_t channel_count = 0;
};

struct OfflineRenderedStem {
    std::string name;
    AudioFileData audio;
};

struct OfflineRenderStemResult {
    AudioFileData mix;
    std::vector<OfflineRenderedStem> stems;
};

struct OfflineRenderComparison {
    uint32_t channels = 0;
    uint64_t frames = 0;
    float peak_error = 0.0f;
    double rms_error = 0.0;

    bool passes(float peak_tolerance, double rms_tolerance) const noexcept;
};

struct OfflineRenderManifestChunk {
    uint64_t start_frame = 0;
    uint64_t frame_count = 0;
    int scheduled_block_size = 0;
};

struct OfflineRenderArtifactManifest {
    uint32_t format_version = 1;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint64_t frames = 0;
    std::string audio_sha256;
    std::string render_plan_sha256;
    uint64_t start_sample_position = 0;
    double start_position_beats = 0.0;
    double tempo_bpm = 120.0;
    double render_speed_ratio = 1.0;
    uint64_t state_generation = 0;
    uint64_t deterministic_seed = 0;
    OfflineRenderTailPolicy tail_policy = OfflineRenderTailPolicy::Truncate;
    uint64_t tail_frames = 0;
    std::vector<int> block_size_schedule;
    std::vector<OfflineRenderManifestChunk> chunks;

    bool matches_audio(const AudioFileData& audio) const noexcept;
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

/// Render an in-memory file and extract named channel-range stems from the mix.
std::optional<OfflineRenderStemResult> offline_render_stems(
    const AudioFileData& input,
    OfflineRenderCallback render_fn,
    const OfflineRenderOptions& options,
    const std::vector<OfflineRenderStem>& stems
);

/// Compare two rendered artifacts for golden/null tests.
std::optional<OfflineRenderComparison> compare_offline_render_audio(
    const AudioFileData& actual,
    const AudioFileData& expected
);

/// Compute a canonical SHA-256 digest for rendered floating-point audio.
std::optional<std::string> offline_render_audio_sha256(
    const AudioFileData& audio
);

/// Build a deterministic manifest for a rendered offline audio artifact.
std::optional<OfflineRenderArtifactManifest> create_offline_render_manifest(
    const AudioFileData& rendered_audio,
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

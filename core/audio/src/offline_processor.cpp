#include <pulp/audio/offline_processor.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/trace.hpp>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace pulp::audio {

namespace {

bool has_consistent_channel_lengths(const AudioFileData& input) {
    if (input.empty()) return false;

    const auto expected_frames = input.num_frames();
    return std::all_of(input.channels.begin(), input.channels.end(),
                       [expected_frames](const auto& channel) {
                           return channel.size() == expected_frames;
                       });
}

bool has_valid_block_schedule(const OfflineRenderOptions& options) {
    if (options.fallback_block_size <= 0) return false;
    if (options.tempo_bpm <= 0.0) return false;
    if (options.render_speed_ratio <= 0.0) return false;
    for (int block_size : options.block_size_schedule) {
        if (block_size <= 0) return false;
    }
    return true;
}

bool has_valid_stems(const std::vector<OfflineRenderStem>& stems,
                     uint32_t channels) {
    std::vector<bool> claimed(channels, false);
    for (const auto& stem : stems) {
        if (stem.name.empty() || stem.channel_count == 0) return false;
        if (stem.first_channel >= channels) return false;
        if (stem.channel_count > channels - stem.first_channel) return false;
        for (uint32_t channel = stem.first_channel;
             channel < stem.first_channel + stem.channel_count; ++channel) {
            if (claimed[channel]) return false;
            claimed[channel] = true;
        }
    }
    return true;
}

int scheduled_block_size_for(const OfflineRenderOptions& options,
                             uint64_t block_index) {
    if (options.block_size_schedule.empty()) return options.fallback_block_size;
    const size_t schedule_index = static_cast<size_t>(
        std::min<uint64_t>(
            block_index,
            static_cast<uint64_t>(options.block_size_schedule.size() - 1)));
    return options.block_size_schedule[schedule_index];
}

void append_u32(std::string& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<char>((value >> shift) & 0xffu));
}

void append_u64(std::string& bytes, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8)
        bytes.push_back(static_cast<char>((value >> shift) & 0xffu));
}

void append_i32(std::string& bytes, int value) {
    append_u32(bytes, static_cast<uint32_t>(value));
}

void append_bool(std::string& bytes, bool value) {
    bytes.push_back(value ? '\1' : '\0');
}

void append_string(std::string& bytes, const std::string& value) {
    append_u64(bytes, static_cast<uint64_t>(value.size()));
    bytes.append(value);
}

void append_float(std::string& bytes, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(bytes, bits);
}

void append_double(std::string& bytes, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u64(bytes, bits);
}

void append_options(std::string& bytes, const OfflineRenderOptions& options) {
    append_i32(bytes, options.fallback_block_size);
    append_u64(bytes, static_cast<uint64_t>(options.block_size_schedule.size()));
    for (int block_size : options.block_size_schedule)
        append_i32(bytes, block_size);
    append_u64(bytes, options.start_sample_position);
    append_double(bytes, options.start_position_beats);
    append_double(bytes, options.tempo_bpm);
    append_double(bytes, options.render_speed_ratio);
    append_u64(bytes, options.state_generation);
    append_u64(bytes, options.deterministic_seed);
    append_u32(bytes, static_cast<uint32_t>(options.tail_policy));
    append_u64(bytes, options.tail_frames);
}

bool is_valid_resource_hash(const OfflineRenderResourceRef& resource) {
    return resource.content_sha256.size() == 64;
}

std::optional<std::vector<OfflineRenderResourceRef>> normalized_resources(
    const std::vector<OfflineRenderResourceRef>& resources) {
    std::vector<OfflineRenderResourceRef> normalized = resources;
    std::sort(normalized.begin(), normalized.end(),
              [](const auto& a, const auto& b) {
                  if (a.id != b.id) return a.id < b.id;
                  if (a.cache_key != b.cache_key) return a.cache_key < b.cache_key;
                  return a.path < b.path;
              });

    for (size_t i = 0; i < normalized.size(); ++i) {
        const auto& resource = normalized[i];
        if (resource.id.empty()) return std::nullopt;
        if (i > 0 && normalized[i - 1].id == resource.id)
            return std::nullopt;
        if (resource.required && !resource.staged) return std::nullopt;
        if (resource.staged && !is_valid_resource_hash(resource))
            return std::nullopt;
    }
    return normalized;
}

std::string hash_resource_set(
    const std::vector<OfflineRenderResourceRef>& resources) {
    std::string bytes;
    bytes.reserve(32 + resources.size() * 160);
    append_u32(bytes, 1);
    append_u64(bytes, static_cast<uint64_t>(resources.size()));
    for (const auto& resource : resources) {
        append_string(bytes, resource.id);
        append_string(bytes, resource.path);
        append_string(bytes, resource.content_sha256);
        append_string(bytes, resource.cache_key);
        append_u64(bytes, resource.generation);
        append_u64(bytes, resource.decoded_bytes);
        append_bool(bytes, resource.required);
        append_bool(bytes, resource.staged);
    }
    return runtime::sha256_hex(bytes);
}

std::vector<OfflineRenderManifestChunk> build_manifest_chunks(
    uint64_t frames,
    const OfflineRenderOptions& options) {
    std::vector<OfflineRenderManifestChunk> chunks;
    uint64_t pos = 0;
    uint64_t block_index = 0;
    while (pos < frames) {
        const int block_size = scheduled_block_size_for(options, block_index);
        const uint64_t frame_count =
            std::min<uint64_t>(static_cast<uint64_t>(block_size), frames - pos);
        chunks.push_back({pos, frame_count, block_size});
        pos += frame_count;
        ++block_index;
    }
    return chunks;
}

std::string hash_render_plan(const AudioFileData& audio,
                             const OfflineRenderOptions& options,
                             const std::vector<OfflineRenderManifestChunk>& chunks,
                             std::string_view resource_set_hash) {
    std::string bytes;
    bytes.reserve(128 + chunks.size() * 24);
    append_u32(bytes, 1);
    append_u32(bytes, audio.sample_rate);
    append_u32(bytes, audio.num_channels());
    append_u64(bytes, audio.num_frames());
    append_options(bytes, options);
    append_u64(bytes, static_cast<uint64_t>(resource_set_hash.size()));
    bytes.append(resource_set_hash);
    append_u64(bytes, static_cast<uint64_t>(chunks.size()));
    for (const auto& chunk : chunks) {
        append_u64(bytes, chunk.start_frame);
        append_u64(bytes, chunk.frame_count);
        append_i32(bytes, chunk.scheduled_block_size);
    }
    return runtime::sha256_hex(bytes);
}

}  // namespace

bool OfflineRenderComparison::passes(float peak_tolerance,
                                     double rms_tolerance) const noexcept {
    return peak_error <= peak_tolerance && rms_error <= rms_tolerance;
}

bool OfflineRenderArtifactManifest::matches_audio(
    const AudioFileData& audio) const noexcept {
    return sample_rate == audio.sample_rate
        && channels == audio.num_channels()
        && frames == audio.num_frames();
}

std::optional<AudioFileData> offline_render(
    const AudioFileData& input,
    OfflineRenderCallback render_fn,
    const OfflineRenderOptions& options)
{
    if (!has_consistent_channel_lengths(input) || !render_fn
        || !has_valid_block_schedule(options)) {
        return std::nullopt;
    }

    // Offline render is deadline-free and deterministic, so Perfetto spans are
    // RT-hazard-free here (unlike the live audio callback — see trace.hpp / plan
    // §0c D1). A whole-render span brackets the block loop; per-block spans
    // below carry the DSP timing the load meter's scalar average hides.
    PULP_TRACE_SCOPE_NAMED("dsp", "offline_render");

    uint32_t channels = input.num_channels();
    const uint64_t input_frames = input.num_frames();
    const uint64_t total_frames =
        input_frames
        + (options.tail_policy == OfflineRenderTailPolicy::RenderTail
            ? options.tail_frames
            : 0);
    for (const auto& channel : input.channels)
        if (channel.size() != static_cast<size_t>(input_frames))
            return std::nullopt;

    AudioFileData output;
    output.sample_rate = input.sample_rate;
    output.channels.resize(channels);
    for (auto& ch : output.channels)
        ch.resize(static_cast<size_t>(total_frames), 0.0f);

    uint64_t pos = 0;
    uint64_t block_index = 0;
    while (pos < total_frames) {
        const int block_size = scheduled_block_size_for(options, block_index);
        int frames_this_block = static_cast<int>(
            std::min(static_cast<uint64_t>(block_size), total_frames - pos));
        std::vector<float> in_block(
            static_cast<size_t>(block_size) * channels, 0.0f);
        std::vector<float> out_block(
            static_cast<size_t>(block_size) * channels, 0.0f);

        // Interleave input
        for (int f = 0; f < frames_this_block; ++f) {
            const uint64_t source_frame = pos + static_cast<uint64_t>(f);
            for (uint32_t ch = 0; ch < channels; ++ch) {
                in_block[static_cast<size_t>(f) * channels + ch] =
                    source_frame < input_frames
                        ? input.channels[ch][static_cast<size_t>(source_frame)]
                        : 0.0f;
            }
        }

        // Zero remaining samples in last block
        if (frames_this_block < block_size) {
            std::memset(in_block.data() + frames_this_block * channels, 0,
                        static_cast<size_t>(block_size - frames_this_block) * channels * sizeof(float));
        }

        OfflineRenderBlockContext context;
        context.block_index = block_index;
        context.sample_position = options.start_sample_position + pos;
        context.frames = frames_this_block;
        context.scheduled_block_size = block_size;
        context.sample_rate = static_cast<double>(input.sample_rate);
        context.time_seconds =
            static_cast<double>(context.sample_position)
            / context.sample_rate;
        context.tempo_bpm = options.tempo_bpm;
        context.position_beats =
            options.start_position_beats
            + (static_cast<double>(pos) / context.sample_rate)
                * (options.tempo_bpm / 60.0);
        context.render_speed_ratio = options.render_speed_ratio;
        context.state_generation = options.state_generation;
        context.deterministic_seed = options.deterministic_seed;

        {
            // One span per rendered block, carrying the block index and sample
            // position as args (read from SQL via EXTRACT_ARG). The counter
            // tracks block size so a trace also shows the schedule. The user
            // render_fn adds its own `dsp.node` spans (static names) for
            // per-node attribution.
            PULP_TRACE_SCOPE_NAMED_ARGS(
                "dsp", "offline_block",
                "block_index", static_cast<int64_t>(context.block_index),
                "position_samples",
                static_cast<int64_t>(context.sample_position));
            PULP_TRACE_COUNTER("dsp", "block_frames", frames_this_block);
            render_fn(in_block.data(), out_block.data(),
                      static_cast<int>(channels), context);
        }

        // Deinterleave output
        for (int f = 0; f < frames_this_block; ++f)
            for (uint32_t ch = 0; ch < channels; ++ch)
                output.channels[ch][static_cast<size_t>(pos) + f] =
                    out_block[static_cast<size_t>(f) * channels + ch];

        pos += static_cast<uint64_t>(frames_this_block);
        ++block_index;
    }

    return output;
}

std::optional<OfflineRenderStemResult> offline_render_stems(
    const AudioFileData& input,
    OfflineRenderCallback render_fn,
    const OfflineRenderOptions& options,
    const std::vector<OfflineRenderStem>& stems)
{
    if (!has_consistent_channel_lengths(input)
        || !has_valid_stems(stems, input.num_channels())) {
        return std::nullopt;
    }

    auto mix = offline_render(input, std::move(render_fn), options);
    if (!mix) return std::nullopt;

    OfflineRenderStemResult result;
    result.mix = std::move(*mix);
    result.stems.reserve(stems.size());

    for (const auto& stem : stems) {
        OfflineRenderedStem rendered;
        rendered.name = stem.name;
        rendered.audio.sample_rate = result.mix.sample_rate;
        rendered.audio.channels.reserve(stem.channel_count);
        for (uint32_t channel = stem.first_channel;
             channel < stem.first_channel + stem.channel_count; ++channel) {
            rendered.audio.channels.push_back(
                result.mix.channels[static_cast<size_t>(channel)]);
        }
        result.stems.push_back(std::move(rendered));
    }

    return result;
}

std::optional<OfflineRenderComparison> compare_offline_render_audio(
    const AudioFileData& actual,
    const AudioFileData& expected)
{
    if (!has_consistent_channel_lengths(actual)
        || !has_consistent_channel_lengths(expected)
        || actual.sample_rate != expected.sample_rate
        || actual.num_channels() != expected.num_channels()
        || actual.num_frames() != expected.num_frames()) {
        return std::nullopt;
    }

    OfflineRenderComparison comparison;
    comparison.channels = actual.num_channels();
    comparison.frames = actual.num_frames();

    double sum_squares = 0.0;
    uint64_t sample_count = 0;
    for (uint32_t channel = 0; channel < actual.num_channels(); ++channel) {
        const auto& actual_channel = actual.channels[channel];
        const auto& expected_channel = expected.channels[channel];
        for (size_t frame = 0; frame < actual_channel.size(); ++frame) {
            const float error = actual_channel[frame] - expected_channel[frame];
            const float abs_error = std::abs(error);
            comparison.peak_error =
                std::max(comparison.peak_error, abs_error);
            sum_squares += static_cast<double>(error)
                * static_cast<double>(error);
            ++sample_count;
        }
    }

    comparison.rms_error =
        sample_count == 0
            ? 0.0
            : std::sqrt(sum_squares / static_cast<double>(sample_count));
    return comparison;
}

std::optional<std::string> offline_render_audio_sha256(
    const AudioFileData& audio)
{
    if (!has_consistent_channel_lengths(audio)) return std::nullopt;

    std::string bytes;
    const uint64_t frames = audio.num_frames();
    const uint32_t channels = audio.num_channels();
    bytes.reserve(static_cast<size_t>(
        16 + frames * static_cast<uint64_t>(channels) * sizeof(float)));
    append_u32(bytes, audio.sample_rate);
    append_u32(bytes, channels);
    append_u64(bytes, frames);
    for (uint64_t frame = 0; frame < frames; ++frame)
        for (uint32_t channel = 0; channel < channels; ++channel)
            append_float(bytes, audio.channels[channel][static_cast<size_t>(frame)]);

    return runtime::sha256_hex(bytes);
}

std::optional<OfflineRenderArtifactManifest> create_offline_render_manifest(
    const AudioFileData& rendered_audio,
    const OfflineRenderOptions& options)
{
    if (!has_consistent_channel_lengths(rendered_audio)
        || !has_valid_block_schedule(options)) {
        return std::nullopt;
    }

    auto resources = normalized_resources(options.resources);
    if (!resources) return std::nullopt;

    auto audio_hash = offline_render_audio_sha256(rendered_audio);
    if (!audio_hash) return std::nullopt;

    OfflineRenderArtifactManifest manifest;
    manifest.sample_rate = rendered_audio.sample_rate;
    manifest.channels = rendered_audio.num_channels();
    manifest.frames = rendered_audio.num_frames();
    manifest.audio_sha256 = std::move(*audio_hash);
    manifest.start_sample_position = options.start_sample_position;
    manifest.start_position_beats = options.start_position_beats;
    manifest.tempo_bpm = options.tempo_bpm;
    manifest.render_speed_ratio = options.render_speed_ratio;
    manifest.state_generation = options.state_generation;
    manifest.deterministic_seed = options.deterministic_seed;
    manifest.tail_policy = options.tail_policy;
    manifest.tail_frames = options.tail_frames;
    manifest.block_size_schedule = options.block_size_schedule;
    manifest.resources = std::move(*resources);
    manifest.chunks = build_manifest_chunks(manifest.frames, options);
    manifest.resource_set_sha256 = hash_resource_set(manifest.resources);
    manifest.cache_reusable = true;
    for (const auto& resource : manifest.resources) {
        if (!resource.staged) ++manifest.missing_optional_resources;
        if (!resource.staged || !is_valid_resource_hash(resource)
            || resource.cache_key.empty()) {
            manifest.cache_reusable = false;
        }
    }
    manifest.render_plan_sha256 =
        hash_render_plan(rendered_audio, options, manifest.chunks,
                         manifest.resource_set_sha256);
    return manifest;
}

OfflineRenderComputeDecision evaluate_offline_render_compute_policy(
    const OfflineRenderComputePolicy& policy)
{
    if (policy.requested_backend == OfflineRenderComputeBackend::Cpu) {
        return {true, OfflineRenderComputeBackend::Cpu, false, "cpu"};
    }

    if (policy.scope == OfflineRenderExecutionScope::RealtimeAudioThread) {
        return {
            false,
            OfflineRenderComputeBackend::Cpu,
            false,
            "gpu-not-allowed-on-realtime-audio-thread",
        };
    }

    if (policy.gpu_available) {
        return {true, OfflineRenderComputeBackend::Gpu, false, "gpu"};
    }

    if (policy.allow_cpu_fallback) {
        return {
            true,
            OfflineRenderComputeBackend::Cpu,
            true,
            "gpu-unavailable-cpu-fallback",
        };
    }

    return {
        false,
        OfflineRenderComputeBackend::Cpu,
        false,
        "gpu-unavailable",
    };
}

std::optional<AudioFileData> offline_process(
    const AudioFileData& input,
    OfflineProcessCallback process_fn,
    int block_size)
{
    if (!process_fn) return std::nullopt;

    OfflineRenderOptions options;
    options.fallback_block_size = block_size;
    return offline_render(
        input,
        [&](const float* in, float* out, int channels,
            const OfflineRenderBlockContext& context) {
            process_fn(in, out, channels, context.frames, context.sample_rate);
        },
        options);
}

bool offline_process_file(
    const std::string& input_path,
    const std::string& output_path,
    OfflineProcessCallback process_fn,
    int block_size)
{
    auto& registry = FormatRegistry::instance();

    auto input = registry.read(input_path);
    if (!input) return false;

    auto output = offline_process(*input, process_fn, block_size);
    if (!output) return false;

    return registry.write(output_path, *output);
}

AudioFileData apply_gain(const AudioFileData& input, float gain_linear) {
    AudioFileData output;
    output.sample_rate = input.sample_rate;
    output.channels.resize(input.num_channels());

    for (uint32_t ch = 0; ch < input.num_channels(); ++ch) {
        output.channels[ch].resize(input.channels[ch].size());
        for (size_t i = 0; i < input.channels[ch].size(); ++i)
            output.channels[ch][i] = input.channels[ch][i] * gain_linear;
    }

    return output;
}

}  // namespace pulp::audio

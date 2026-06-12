#include <pulp/audio/offline_processor.hpp>
#include <pulp/audio/format_registry.hpp>
#include <vector>
#include <algorithm>
#include <cstring>

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
    for (int block_size : options.block_size_schedule) {
        if (block_size <= 0) return false;
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

}  // namespace

std::optional<AudioFileData> offline_render(
    const AudioFileData& input,
    OfflineRenderCallback render_fn,
    const OfflineRenderOptions& options)
{
    if (!has_consistent_channel_lengths(input) || !render_fn
        || !has_valid_block_schedule(options)) {
        return std::nullopt;
    }

    uint32_t channels = input.num_channels();
    uint64_t total_frames = input.num_frames();
    for (const auto& channel : input.channels)
        if (channel.size() != static_cast<size_t>(total_frames))
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
        for (int f = 0; f < frames_this_block; ++f)
            for (uint32_t ch = 0; ch < channels; ++ch)
                in_block[static_cast<size_t>(f) * channels + ch] =
                    input.channels[ch][static_cast<size_t>(pos) + f];

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
        context.deterministic_seed = options.deterministic_seed;

        render_fn(in_block.data(), out_block.data(), static_cast<int>(channels),
                  context);

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

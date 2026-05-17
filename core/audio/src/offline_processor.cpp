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

}  // namespace

std::optional<AudioFileData> offline_process(
    const AudioFileData& input,
    OfflineProcessCallback process_fn,
    int block_size)
{
    if (!has_consistent_channel_lengths(input) || !process_fn || block_size <= 0)
        return std::nullopt;

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

    // Process in blocks
    std::vector<float> in_block(static_cast<size_t>(block_size) * channels, 0.0f);
    std::vector<float> out_block(static_cast<size_t>(block_size) * channels, 0.0f);

    uint64_t pos = 0;
    while (pos < total_frames) {
        int frames_this_block = static_cast<int>(
            std::min(static_cast<uint64_t>(block_size), total_frames - pos));

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

        std::memset(out_block.data(), 0, out_block.size() * sizeof(float));

        // Process
        process_fn(in_block.data(), out_block.data(), static_cast<int>(channels),
                   frames_this_block, static_cast<double>(input.sample_rate));

        // Deinterleave output
        for (int f = 0; f < frames_this_block; ++f)
            for (uint32_t ch = 0; ch < channels; ++ch)
                output.channels[ch][static_cast<size_t>(pos) + f] =
                    out_block[static_cast<size_t>(f) * channels + ch];

        pos += static_cast<uint64_t>(frames_this_block);
    }

    return output;
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

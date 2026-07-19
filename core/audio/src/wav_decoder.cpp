#include <pulp/audio/wav_decoder.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>

#define DRWAV_API [[maybe_unused]] static
#define DRWAV_PRIVATE static
#define DR_WAV_LIBSNDFILE_COMPAT
#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

namespace pulp::audio {
namespace {

constexpr std::size_t kInterleavedChunkSamples = 4'096u;

struct RiffLayout {
    std::size_t format_offset = 0u;
    std::size_t format_extent = 0u;
    std::size_t data_offset = 0u;
    std::size_t data_size = 0u;
};

struct RiffReader {
    std::array<std::span<const std::uint8_t>, 4u> fragments{};
    std::size_t fragment_count = 0u;
    std::size_t size = 0u;
    std::size_t position = 0u;
};

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

bool has_id(std::span<const std::uint8_t> bytes, std::size_t offset, const char (&id)[5]) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4u)
        return false;
    for (std::size_t i = 0; i < 4u; ++i) {
        if (bytes[offset + i] != static_cast<std::uint8_t>(id[i]))
            return false;
    }
    return true;
}

std::optional<RiffLayout> inspect_riff_layout(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < 12u || !has_id(bytes, 0u, "RIFF") || !has_id(bytes, 8u, "WAVE"))
        return std::nullopt;
    const auto declared_end = static_cast<std::uint64_t>(read_u32(bytes, 4u)) + 8u;
    if (declared_end < 12u || declared_end != bytes.size())
        return std::nullopt;

    RiffLayout layout;
    bool found_format = false;
    bool found_data = false;
    std::size_t offset = 12u;
    const auto end = static_cast<std::size_t>(declared_end);
    while (offset < end) {
        if (end - offset < 8u)
            return std::nullopt;
        const auto chunk_size = static_cast<std::size_t>(read_u32(bytes, offset + 4u));
        const auto payload = offset + 8u;
        if (chunk_size > end - payload)
            return std::nullopt;
        const auto chunk_end = payload + chunk_size;
        const auto padding = chunk_size & 1u;
        if (padding > end - chunk_end)
            return std::nullopt;
        if (!found_format && has_id(bytes, offset, "fmt ")) {
            layout.format_offset = offset;
            layout.format_extent = 8u + chunk_size + padding;
            found_format = true;
        } else if (!found_data && has_id(bytes, offset, "data")) {
            layout.data_offset = offset;
            layout.data_size = chunk_size;
            found_data = true;
        }
        offset = chunk_end + padding;
    }
    if (!found_format || !found_data)
        return std::nullopt;
    return layout;
}

RiffReader make_riff_reader(std::span<const std::uint8_t> bytes,
                            const RiffLayout& layout) noexcept {
    RiffReader reader;
    reader.size = bytes.size();
    if (layout.data_offset < layout.format_offset) {
        const auto format_end = layout.format_offset + layout.format_extent;
        reader.fragments[0] = bytes.first(12u);
        reader.fragments[1] = bytes.subspan(layout.format_offset, layout.format_extent);
        reader.fragments[2] = bytes.subspan(12u, layout.format_offset - 12u);
        reader.fragments[3] = bytes.subspan(format_end);
        reader.fragment_count = 4u;
    } else {
        reader.fragments[0] = bytes;
        reader.fragment_count = 1u;
    }
    return reader;
}

std::size_t read_riff(void* user_data, void* output, std::size_t requested) noexcept {
    auto& reader = *static_cast<RiffReader*>(user_data);
    const auto available = reader.size - reader.position;
    const auto to_read = std::min(requested, available);
    auto* destination = static_cast<std::uint8_t*>(output);
    std::size_t copied = 0u;
    std::size_t fragment_start = 0u;
    for (std::size_t i = 0u; i < reader.fragment_count && copied < to_read; ++i) {
        const auto fragment = reader.fragments[i];
        const auto fragment_end = fragment_start + fragment.size();
        if (reader.position + copied < fragment_end) {
            const auto local_offset = reader.position + copied - fragment_start;
            const auto count = std::min(to_read - copied, fragment.size() - local_offset);
            std::memcpy(destination + copied, fragment.data() + local_offset, count);
            copied += count;
        }
        fragment_start = fragment_end;
    }
    reader.position += copied;
    return copied;
}

drwav_bool32 seek_riff(void* user_data, int offset, drwav_seek_origin origin) noexcept {
    auto& reader = *static_cast<RiffReader*>(user_data);
    const auto base = origin == DRWAV_SEEK_SET ? 0u : reader.position;
    const auto distance = static_cast<std::size_t>(offset);
    if (distance > reader.size - base)
        return DRWAV_FALSE;
    reader.position = base + distance;
    return DRWAV_TRUE;
}

bool init_wav(drwav& wav, RiffReader& reader) noexcept {
    return drwav_init(&wav, read_riff, seek_riff, nullptr, &reader, nullptr) != DRWAV_FALSE;
}

bool has_complete_frames(drwav& wav, const RiffLayout& layout) noexcept {
    if (drwav__is_compressed_format_tag(wav.translatedFormatTag))
        return true;
    const auto bytes_per_frame = drwav_get_bytes_per_pcm_frame(&wav);
    return bytes_per_frame != 0u && layout.data_size % bytes_per_frame == 0u;
}

bool valid_properties(const drwav& wav) noexcept {
    return wav.channels != 0u && wav.sampleRate != 0u && wav.totalPCMFrameCount != 0u;
}

bool checked_output_bytes(std::uint64_t frames, std::uint32_t channels,
                          std::uint64_t& bytes) noexcept {
    constexpr auto kFloatBytes = static_cast<std::uint64_t>(sizeof(float));
    if (channels == 0u || frames > std::numeric_limits<std::uint64_t>::max() / channels)
        return false;
    const auto samples = frames * channels;
    if (samples > std::numeric_limits<std::uint64_t>::max() / kFloatBytes)
        return false;
    bytes = samples * kFloatBytes;
    return true;
}

} // namespace

std::optional<AudioFileInfo> inspect_wav(std::span<const std::uint8_t> bytes) {
    const auto layout = inspect_riff_layout(bytes);
    if (!layout)
        return std::nullopt;

    auto reader = make_riff_reader(bytes, *layout);
    drwav wav{};
    if (!init_wav(wav, reader))
        return std::nullopt;
    if (!valid_properties(wav) || !has_complete_frames(wav, *layout)) {
        drwav_uninit(&wav);
        return std::nullopt;
    }

    AudioFileInfo info;
    info.sample_rate = wav.sampleRate;
    info.num_channels = wav.channels;
    info.num_frames = wav.totalPCMFrameCount;
    info.bits_per_sample = wav.bitsPerSample;
    info.format = "WAV";
    info.duration_seconds =
        static_cast<double>(wav.totalPCMFrameCount) / static_cast<double>(wav.sampleRate);
    drwav_uninit(&wav);
    return info;
}

std::optional<AudioFileData> decode_wav(std::span<const std::uint8_t> bytes,
                                        WavDecodeLimits limits) {
    const auto layout = inspect_riff_layout(bytes);
    if (!layout || limits.max_frames == 0u || limits.max_channels == 0u ||
        limits.max_output_bytes == 0u)
        return std::nullopt;

    auto reader = make_riff_reader(bytes, *layout);
    drwav wav{};
    if (!init_wav(wav, reader))
        return std::nullopt;
    if (!valid_properties(wav) || !has_complete_frames(wav, *layout) ||
        wav.totalPCMFrameCount > limits.max_frames || wav.channels > limits.max_channels ||
        wav.channels > kInterleavedChunkSamples) {
        drwav_uninit(&wav);
        return std::nullopt;
    }

    std::uint64_t output_bytes = 0u;
    if (!checked_output_bytes(wav.totalPCMFrameCount, wav.channels, output_bytes) ||
        output_bytes > limits.max_output_bytes ||
        output_bytes > std::numeric_limits<std::size_t>::max() ||
        wav.totalPCMFrameCount > std::numeric_limits<std::size_t>::max()) {
        drwav_uninit(&wav);
        return std::nullopt;
    }

    const auto frames = static_cast<std::size_t>(wav.totalPCMFrameCount);
    const auto channels = static_cast<std::size_t>(wav.channels);
    AudioFileData decoded;
    decoded.sample_rate = wav.sampleRate;
    decoded.channels.resize(channels);
    for (auto& channel : decoded.channels)
        channel.resize(frames);

    std::array<float, kInterleavedChunkSamples> interleaved{};
    const auto chunk_frames = kInterleavedChunkSamples / channels;
    std::uint64_t frame_offset = 0u;
    while (frame_offset < wav.totalPCMFrameCount) {
        const auto remaining = wav.totalPCMFrameCount - frame_offset;
        const auto requested = remaining < chunk_frames ? remaining : chunk_frames;
        const auto read = drwav_read_pcm_frames_f32(&wav, requested, interleaved.data());
        if (read != requested) {
            drwav_uninit(&wav);
            return std::nullopt;
        }
        for (std::size_t frame = 0; frame < static_cast<std::size_t>(read); ++frame) {
            for (std::size_t channel = 0; channel < channels; ++channel) {
                decoded.channels[channel][static_cast<std::size_t>(frame_offset) + frame] =
                    interleaved[frame * channels + channel];
            }
        }
        frame_offset += read;
    }

    drwav_uninit(&wav);
    return decoded;
}

} // namespace pulp::audio

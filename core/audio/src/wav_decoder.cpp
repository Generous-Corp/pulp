#include <pulp/audio/wav_decoder.hpp>

#include <array>
#include <cstddef>
#include <limits>

#define DRWAV_API [[maybe_unused]] static
#define DRWAV_PRIVATE static
#define DR_WAV_LIBSNDFILE_COMPAT
#define DR_WAV_NO_STDIO
#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

namespace pulp::audio {
namespace {

constexpr std::size_t kInterleavedChunkSamples = 4'096u;

std::uint32_t read_u32(std::span<const std::uint8_t> bytes,
                       std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<std::uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[offset + 3u]) << 24u);
}

bool has_id(std::span<const std::uint8_t> bytes, std::size_t offset,
            const char (&id)[5]) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4u)
        return false;
    for (std::size_t i = 0; i < 4u; ++i) {
        if (bytes[offset + i] != static_cast<std::uint8_t>(id[i]))
            return false;
    }
    return true;
}

// dr_wav deliberately tolerates some short streams by clamping a data chunk to
// the available bytes. Imports need the stronger contract that every size in
// the RIFF envelope is actually present before allocation or decode begins.
bool complete_riff(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < 12u || !has_id(bytes, 0u, "RIFF") ||
        !has_id(bytes, 8u, "WAVE"))
        return false;
    const auto declared_end = static_cast<std::uint64_t>(read_u32(bytes, 4u)) + 8u;
    if (declared_end < 12u || declared_end != bytes.size())
        return false;

    std::size_t offset = 12u;
    const auto end = static_cast<std::size_t>(declared_end);
    while (offset < end) {
        if (end - offset < 8u)
            return false;
        const auto chunk_size = static_cast<std::size_t>(read_u32(bytes, offset + 4u));
        const auto payload = offset + 8u;
        if (chunk_size > end - payload)
            return false;
        const auto chunk_end = payload + chunk_size;
        const auto padding = chunk_size & 1u;
        if (padding > end - chunk_end)
            return false;
        offset = chunk_end + padding;
    }
    return true;
}

bool valid_properties(const drwav& wav) noexcept {
    return wav.channels != 0u && wav.sampleRate != 0u &&
           wav.totalPCMFrameCount != 0u;
}

bool checked_output_bytes(std::uint64_t frames, std::uint32_t channels,
                          std::uint64_t& bytes) noexcept {
    constexpr auto kFloatBytes = static_cast<std::uint64_t>(sizeof(float));
    if (channels == 0u ||
        frames > std::numeric_limits<std::uint64_t>::max() / channels)
        return false;
    const auto samples = frames * channels;
    if (samples > std::numeric_limits<std::uint64_t>::max() / kFloatBytes)
        return false;
    bytes = samples * kFloatBytes;
    return true;
}

} // namespace

std::optional<AudioFileInfo> inspect_wav(std::span<const std::uint8_t> bytes) {
    if (!complete_riff(bytes))
        return std::nullopt;

    drwav wav{};
    if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr))
        return std::nullopt;
    if (!valid_properties(wav)) {
        drwav_uninit(&wav);
        return std::nullopt;
    }

    AudioFileInfo info;
    info.sample_rate = wav.sampleRate;
    info.num_channels = wav.channels;
    info.num_frames = wav.totalPCMFrameCount;
    info.bits_per_sample = wav.bitsPerSample;
    info.format = "WAV";
    info.duration_seconds = static_cast<double>(wav.totalPCMFrameCount) /
                            static_cast<double>(wav.sampleRate);
    drwav_uninit(&wav);
    return info;
}

std::optional<AudioFileData> decode_wav(std::span<const std::uint8_t> bytes,
                                        WavDecodeLimits limits) {
    if (!complete_riff(bytes) || limits.max_frames == 0u ||
        limits.max_channels == 0u || limits.max_output_bytes == 0u)
        return std::nullopt;

    drwav wav{};
    if (!drwav_init_memory(&wav, bytes.data(), bytes.size(), nullptr))
        return std::nullopt;
    if (!valid_properties(wav) || wav.totalPCMFrameCount > limits.max_frames ||
        wav.channels > limits.max_channels ||
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

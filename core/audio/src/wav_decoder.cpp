#include <pulp/audio/wav_decoder.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

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
        // dr_wav stops at the first data chunk unless metadata parsing is
        // enabled. Present fmt first through spans instead of allocating a
        // reordered copy or enabling metadata allocation for hostile input.
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
    if (reader.position > reader.size)
        return 0u;
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
    if (offset < 0)
        return DRWAV_FALSE;
    const auto base = origin == DRWAV_SEEK_SET ? 0u : reader.position;
    const auto distance = static_cast<std::size_t>(offset);
    if (base > reader.size || distance > reader.size - base)
        return DRWAV_FALSE;
    reader.position = base + distance;
    return DRWAV_TRUE;
}

class WavSession {
  public:
    WavSession(std::span<const std::uint8_t> bytes, const RiffLayout& layout) noexcept
        : reader_(make_riff_reader(bytes, layout)),
          initialized_(drwav_init(&wav_, read_riff, seek_riff, nullptr, &reader_, nullptr) !=
                       DRWAV_FALSE) {}

    ~WavSession() {
        if (initialized_)
            drwav_uninit(&wav_);
    }

    WavSession(const WavSession&) = delete;
    WavSession& operator=(const WavSession&) = delete;

    explicit operator bool() const noexcept {
        return initialized_;
    }
    drwav& wav() noexcept {
        return wav_;
    }

  private:
    RiffReader reader_;
    drwav wav_{};
    bool initialized_ = false;
};

bool has_complete_frames(drwav& wav, const RiffLayout& layout) noexcept {
    const auto format = wav.translatedFormatTag;
    if (format == DR_WAVE_FORMAT_ADPCM || format == DR_WAVE_FORMAT_DVI_ADPCM)
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

    WavSession session(bytes, *layout);
    if (!session)
        return std::nullopt;
    auto& wav = session.wav();
    if (!valid_properties(wav) || !has_complete_frames(wav, *layout)) {
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
    return info;
}

std::optional<WavFormatFacts> interpret_wav_format(std::span<const std::uint8_t> fmt_payload,
                                                   std::uint64_t data_size,
                                                   std::optional<std::uint64_t> fact_sample_count) {
    // dr_wav needs only the first 40 bytes of a fmt payload; a larger one is
    // copied up to this cap and declared at the copied length so the
    // synthetic file stays self-consistent.
    constexpr std::size_t kMaxFormatCopy = 1024u;
    constexpr std::uint64_t kRiffSizeSentinel = 0xFFFFFFFFu;
    const auto format_copy = std::min(fmt_payload.size(), kMaxFormatCopy);
    const bool rf64 = data_size >= kRiffSizeSentinel;

    // Build the smallest file dr_wav accepts for these headers: envelope,
    // ds64 when the data size needs 64 bits, fmt, optional fact, and the data
    // chunk header. dr_wav stops at the data header, so no payload is needed.
    std::vector<std::uint8_t> synthetic;
    synthetic.reserve(96u + format_copy);
    auto put_id = [&](const char (&id)[5]) {
        for (std::size_t i = 0; i < 4u; ++i)
            synthetic.push_back(static_cast<std::uint8_t>(id[i]));
    };
    auto put_u32 = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32u; shift += 8u)
            synthetic.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    auto put_u64 = [&](std::uint64_t value) {
        for (unsigned shift = 0; shift < 64u; shift += 8u)
            synthetic.push_back(static_cast<std::uint8_t>(value >> shift));
    };

    put_id(rf64 ? "RF64" : "RIFF");
    const auto riff_size_slot = synthetic.size();
    put_u32(0u);
    put_id("WAVE");
    if (rf64) {
        put_id("ds64");
        put_u32(28u);
        put_u64(0u);         // riff size, patched below
        put_u64(data_size);
        put_u64(fact_sample_count.value_or(0u));
        put_u32(0u);         // no table entries
    }
    put_id("fmt ");
    put_u32(static_cast<std::uint32_t>(format_copy));
    synthetic.insert(synthetic.end(), fmt_payload.begin(),
                     fmt_payload.begin() + static_cast<std::ptrdiff_t>(format_copy));
    if ((format_copy & 1u) != 0u)
        synthetic.push_back(0u);
    if (fact_sample_count) {
        put_id("fact");
        put_u32(4u);
        put_u32(static_cast<std::uint32_t>(std::min<std::uint64_t>(*fact_sample_count, 0xFFFFFFFFu)));
    }
    put_id("data");
    put_u32(static_cast<std::uint32_t>(rf64 ? kRiffSizeSentinel : data_size));

    const auto header_bytes = static_cast<std::uint64_t>(synthetic.size()) - 8u;
    const auto riff_size = header_bytes + data_size;
    const auto riff_size32 =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(riff_size, kRiffSizeSentinel));
    for (unsigned shift = 0; shift < 32u; shift += 8u)
        synthetic[riff_size_slot + shift / 8u] = static_cast<std::uint8_t>(riff_size32 >> shift);
    if (rf64) {
        const auto ds64_riff_slot = 20u;
        for (unsigned shift = 0; shift < 64u; shift += 8u)
            synthetic[ds64_riff_slot + shift / 8u] = static_cast<std::uint8_t>(riff_size >> shift);
    }

    RiffLayout layout;
    WavSession session(synthetic, layout);
    if (!session)
        return std::nullopt;
    auto& wav = session.wav();
    WavFormatFacts facts;
    facts.format_tag = wav.fmt.formatTag;
    facts.translated_format_tag = wav.translatedFormatTag;
    facts.channels = wav.channels;
    facts.sample_rate = wav.sampleRate;
    facts.bits_per_sample = wav.bitsPerSample;
    facts.bytes_per_frame = drwav_get_bytes_per_pcm_frame(&wav);
    facts.total_frames = wav.totalPCMFrameCount;
    return facts;
}

std::optional<AudioFileData> decode_wav(std::span<const std::uint8_t> bytes,
                                        WavDecodeLimits limits) {
    const auto layout = inspect_riff_layout(bytes);
    if (!layout || limits.max_frames == 0u || limits.max_channels == 0u ||
        limits.max_output_bytes == 0u)
        return std::nullopt;

    WavSession session(bytes, *layout);
    if (!session)
        return std::nullopt;
    auto& wav = session.wav();
    if (!valid_properties(wav) || !has_complete_frames(wav, *layout) ||
        wav.totalPCMFrameCount > limits.max_frames || wav.channels > limits.max_channels ||
        wav.channels > kInterleavedChunkSamples) {
        return std::nullopt;
    }

    std::uint64_t output_bytes = 0u;
    if (!checked_output_bytes(wav.totalPCMFrameCount, wav.channels, output_bytes) ||
        output_bytes > limits.max_output_bytes ||
        output_bytes > std::numeric_limits<std::size_t>::max() ||
        wav.totalPCMFrameCount > std::numeric_limits<std::size_t>::max()) {
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

    return decoded;
}

} // namespace pulp::audio

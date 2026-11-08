// OGG Vorbis reader via stb_vorbis
// stb_vorbis is a single-file C library — we include it directly.

#include <pulp/audio/format_registry.hpp>
#include "decode_limits_detail.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// stb_vorbis C implementation
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

namespace pulp::audio {
namespace {

// Setup memory stb_vorbis may take while reading the headers, sized from the
// file. The header lengths (vendor string, comment count, codebook sizes) are
// untrusted 32-bit fields; with an arena the decoder fails with
// VORBIS_outofmem when one exceeds it, instead of asking malloc for gigabytes
// it then touches. Real streams need tens of kilobytes; the floor keeps a tiny
// file's legitimate setup inside the arena and the ceiling bounds the worst
// claim a large file can make.
constexpr std::size_t kSetupArenaFloor = 1u << 20;
constexpr std::size_t kSetupArenaCeiling = 64u << 20;

std::vector<char> setup_arena_for(const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    const std::size_t bytes =
        ec ? kSetupArenaFloor
           : static_cast<std::size_t>(std::min<std::uintmax_t>(size, kSetupArenaCeiling / 32u)) *
                 32u;
    return std::vector<char>(std::clamp(bytes, kSetupArenaFloor, kSetupArenaCeiling));
}

stb_vorbis* open_bounded(const std::string& path, std::vector<char>& arena) {
    stb_vorbis_alloc alloc;
    alloc.alloc_buffer = arena.data();
    alloc.alloc_buffer_length_in_bytes = static_cast<int>(arena.size());
    int error = 0;
    return stb_vorbis_open_filename(path.c_str(), &error, &alloc);
}

} // namespace

class OggReader : public FormatReader {
public:
    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        auto arena = setup_arena_for(path);
        stb_vorbis* v = open_bounded(path, arena);
        if (!v) return std::nullopt;

        stb_vorbis_info info = stb_vorbis_get_info(v);
        unsigned int total = stb_vorbis_stream_length_in_samples(v);

        AudioFileInfo result;
        result.sample_rate = static_cast<uint32_t>(info.sample_rate);
        result.num_channels = static_cast<uint32_t>(info.channels);
        result.num_frames = total;
        result.bits_per_sample = 16;
        result.format = "OGG";
        result.duration_seconds = static_cast<double>(total) / info.sample_rate;

        stb_vorbis_close(v);
        return result;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        return read(path, DecodeLimits{});
    }

    std::optional<AudioFileData> read(const std::string& path,
                                      const DecodeLimits& limits) override {
        auto arena = setup_arena_for(path);
        stb_vorbis* v = open_bounded(path, arena);
        if (!v)
            return std::nullopt;

        const stb_vorbis_info info = stb_vorbis_get_info(v);
        if (info.channels <= 0) {
            stb_vorbis_close(v);
            return std::nullopt;
        }
        const auto channels = static_cast<uint32_t>(info.channels);

        // The last page's granule position is the declared length, zero when
        // the last page cannot be found. That probe leaves
        // VORBIS_cant_find_last_page set and nothing clears it, so the error
        // is read here and the one read after decoding measures the decode.
        const uint64_t claimed_frames = stb_vorbis_stream_length_in_samples(v);
        (void)stb_vorbis_get_error(v);

        // Decode as int16 and scale, matching stb_vorbis's whole-file decode.
        std::vector<short> shorts(static_cast<size_t>(detail::kDecodeChunkFrames) * channels);
        auto data = detail::decode_chunked(
            "OGG Vorbis", static_cast<uint32_t>(info.sample_rate), channels, claimed_frames, limits,
            [&](float* out, uint64_t frames) -> uint64_t {
                const int got = stb_vorbis_get_samples_short_interleaved(
                    v, info.channels, shorts.data(), static_cast<int>(frames * channels));
                if (got <= 0)
                    return 0u;
                const size_t samples = static_cast<size_t>(got) * channels;
                for (size_t i = 0; i < samples; ++i)
                    out[i] = static_cast<float>(shorts[i]) / 32768.0f;
                return static_cast<uint64_t>(got);
            });
        // With no declared length the stream's own end is the only evidence
        // of completeness, so a decoder error there is a refusal.
        const int decode_error = stb_vorbis_get_error(v);
        stb_vorbis_close(v);
        if (claimed_frames == 0u && decode_error != VORBIS__no_error)
            return std::nullopt;
        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".ogg" || ext == ".oga";
    }
    std::string format_name() const override { return "OGG Vorbis"; }
    FormatCapabilities capabilities() const override {
        return {.format_name = "OGG Vorbis",
                .extensions = {".ogg", ".oga"},
                .max_channels = 255,
                .compressed = true,
                .lossless = false};
    }
};

std::unique_ptr<FormatReader> create_ogg_reader() {
    return std::make_unique<OggReader>();
}

}  // namespace pulp::audio

// OGG Vorbis reader via stb_vorbis
// stb_vorbis is a single-file C library — we include it directly.

#include <pulp/audio/format_registry.hpp>
#include "decode_limits_detail.hpp"
#include <cstdint>
#include <vector>

// stb_vorbis C implementation
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

namespace pulp::audio {

class OggReader : public FormatReader {
public:
    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        int error = 0;
        stb_vorbis* v = stb_vorbis_open_filename(path.c_str(), &error, nullptr);
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
        int error = 0;
        stb_vorbis* v = stb_vorbis_open_filename(path.c_str(), &error, nullptr);
        if (!v)
            return std::nullopt;

        const stb_vorbis_info info = stb_vorbis_get_info(v);
        if (info.channels <= 0) {
            stb_vorbis_close(v);
            return std::nullopt;
        }
        const auto channels = static_cast<uint32_t>(info.channels);

        // The last page's granule position is an untrusted length claim
        // (zero when unknown); refuse before any output allocation.
        const uint64_t claimed_frames = stb_vorbis_stream_length_in_samples(v);
        if (!detail::within_decode_limits("OGG Vorbis", claimed_frames, channels, limits)) {
            stb_vorbis_close(v);
            return std::nullopt;
        }

        AudioFileData data;
        data.sample_rate = static_cast<uint32_t>(info.sample_rate);
        data.channels.resize(channels);
        detail::reserve_planar(data.channels, claimed_frames);

        // Decode as int16 and scale, matching stb_vorbis's whole-file decode.
        std::vector<short> interleaved(static_cast<size_t>(detail::kDecodeChunkFrames) * channels);
        std::vector<float> chunk(interleaved.size());
        uint64_t frames_read = 0;
        bool within_limits = true;
        for (;;) {
            const int got = stb_vorbis_get_samples_short_interleaved(
                v, info.channels, interleaved.data(), static_cast<int>(interleaved.size()));
            if (got <= 0)
                break;
            const auto got_frames = static_cast<uint64_t>(got);
            if (!detail::within_decode_limits("OGG Vorbis", frames_read + got_frames, channels,
                                              limits)) {
                within_limits = false;
                break;
            }
            const size_t samples = static_cast<size_t>(got_frames) * channels;
            for (size_t i = 0; i < samples; ++i)
                chunk[i] = static_cast<float>(interleaved[i]) / 32768.0f;
            detail::append_deinterleaved(data.channels, chunk.data(), got_frames, channels);
            frames_read += got_frames;
        }
        stb_vorbis_close(v);

        if (!within_limits || frames_read == 0)
            return std::nullopt;
        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".ogg" || ext == ".oga";
    }
    std::string format_name() const override { return "OGG Vorbis"; }
};

std::unique_ptr<FormatReader> create_ogg_reader() {
    return std::make_unique<OggReader>();
}

}  // namespace pulp::audio

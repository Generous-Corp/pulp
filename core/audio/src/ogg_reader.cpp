// OGG Vorbis reader via stb_vorbis
// stb_vorbis is a single-file C library — we include it directly.

#include <pulp/audio/format_registry.hpp>
#include <cstdlib>

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
        int channels = 0, sample_rate = 0;
        short* samples = nullptr;
        int total_frames = stb_vorbis_decode_filename(path.c_str(), &channels, &sample_rate, &samples);

        if (total_frames <= 0 || !samples) {
            if (samples) std::free(samples);
            return std::nullopt;
        }

        AudioFileData data;
        data.sample_rate = static_cast<uint32_t>(sample_rate);
        data.channels.resize(static_cast<size_t>(channels));
        for (auto& ch : data.channels)
            ch.resize(static_cast<size_t>(total_frames));

        // Deinterleave and convert int16 -> float
        for (int i = 0; i < total_frames; ++i)
            for (int ch = 0; ch < channels; ++ch)
                data.channels[ch][i] = static_cast<float>(samples[i * channels + ch]) / 32768.0f;

        std::free(samples);
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

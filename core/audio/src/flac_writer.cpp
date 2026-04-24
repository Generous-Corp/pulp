// FLAC writer — lossless audio encoding via libflac.
// Conditionally compiled: only when libflac is installed (pulp add libflac).
// Registered explicitly by FormatRegistry when PULP_HAS_LIBFLAC is enabled.

#ifdef PULP_HAS_LIBFLAC

#include <algorithm>
#include <pulp/audio/format_registry.hpp>
#include <FLAC/stream_encoder.h>
#include <vector>
#include <cmath>

namespace pulp::audio {

class FlacWriter : public FormatWriter {
public:
    bool write(const std::string& path, const AudioFileData& data) override {
        if (data.empty()) return false;

        FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
        if (!encoder) return false;

        uint32_t channels = data.num_channels();
        uint32_t total_frames = static_cast<uint32_t>(data.num_frames());
        uint32_t bits = 16;

        FLAC__stream_encoder_set_channels(encoder, channels);
        FLAC__stream_encoder_set_bits_per_sample(encoder, bits);
        FLAC__stream_encoder_set_sample_rate(encoder, data.sample_rate);
        FLAC__stream_encoder_set_compression_level(encoder, 5);
        FLAC__stream_encoder_set_total_samples_estimate(encoder, total_frames);

        FLAC__StreamEncoderInitStatus status =
            FLAC__stream_encoder_init_file(encoder, path.c_str(), nullptr, nullptr);

        if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
            FLAC__stream_encoder_delete(encoder);
            return false;
        }

        // Convert float to int32 samples (interleaved by channel per frame)
        std::vector<FLAC__int32> samples(static_cast<size_t>(total_frames) * channels);
        for (uint32_t f = 0; f < total_frames; ++f)
            for (uint32_t c = 0; c < channels; ++c)
                samples[f * channels + c] = static_cast<FLAC__int32>(
                    std::clamp(data.channels[c][f], -1.0f, 1.0f) * 32767.0f);

        // Encode in blocks
        const FLAC__int32* buffer[8] = {};
        std::vector<std::vector<FLAC__int32>> channel_bufs(channels);
        for (uint32_t c = 0; c < channels; ++c) {
            channel_bufs[c].resize(total_frames);
            for (uint32_t f = 0; f < total_frames; ++f)
                channel_bufs[c][f] = samples[f * channels + c];
            buffer[c] = channel_bufs[c].data();
        }

        bool ok = FLAC__stream_encoder_process(encoder, buffer, total_frames);

        FLAC__stream_encoder_finish(encoder);
        FLAC__stream_encoder_delete(encoder);
        return ok;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".flac";
    }

    std::string format_name() const override { return "FLAC"; }
};

std::unique_ptr<FormatWriter> create_flac_writer() {
    return std::make_unique<FlacWriter>();
}

}  // namespace pulp::audio

#endif  // PULP_HAS_LIBFLAC

// ALAC writer — Apple Lossless Audio Codec encoding.
// Conditionally compiled: only when Apple ALAC is installed (pulp add alac).
// Auto-registers AlacWriter in FormatRegistry via static initializer.
//
// Note: On macOS, CoreAudioFormat can also write ALAC via ExtAudioFile.
// This implementation provides cross-platform ALAC write using Apple's
// open-source reference encoder (Apache 2.0).

#ifdef PULP_HAS_ALAC

#include <pulp/audio/format_registry.hpp>
#include <ALACEncoder.h>
#include <ALACBitUtilities.h>
#include <vector>
#include <fstream>
#include <cmath>

namespace pulp::audio {

class AlacWriter : public FormatWriter {
public:
    bool write(const std::string& path, const AudioFileData& data) override {
        if (data.empty()) return false;

        uint32_t channels = data.num_channels();
        uint32_t total_frames = static_cast<uint32_t>(data.num_frames());

        ALACEncoder encoder;
        AudioFormatDescription input_format = {};
        input_format.mSampleRate = static_cast<Float64>(data.sample_rate);
        input_format.mFormatID = kALACFormatLinearPCM;
        input_format.mBitsPerChannel = 16;
        input_format.mChannelsPerFrame = channels;
        input_format.mBytesPerFrame = 2 * channels;
        input_format.mFramesPerPacket = 1;
        input_format.mBytesPerPacket = input_format.mBytesPerFrame;

        AudioFormatDescription output_format = {};
        output_format.mSampleRate = input_format.mSampleRate;
        output_format.mFormatID = kALACFormatAppleLossless;
        output_format.mChannelsPerFrame = channels;
        output_format.mFramesPerPacket = 4096;

        int32_t status = encoder.InitializeEncoder(output_format);
        if (status != ALAC_noErr) return false;

        // Convert float to int16 interleaved
        std::vector<int16_t> pcm(static_cast<size_t>(total_frames) * channels);
        for (uint32_t f = 0; f < total_frames; ++f)
            for (uint32_t c = 0; c < channels; ++c)
                pcm[f * channels + c] = static_cast<int16_t>(
                    std::clamp(data.channels[c][f], -1.0f, 1.0f) * 32767.0f);

        // Encode to raw ALAC packets
        // Note: A proper M4A container would need an MP4 muxer.
        // This writes raw ALAC packets — usable but not a standard .m4a file.
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;

        uint32_t frames_per_packet = 4096;
        std::vector<uint8_t> out_buf(frames_per_packet * channels * 4);

        for (uint32_t pos = 0; pos < total_frames; pos += frames_per_packet) {
            uint32_t frames_this = std::min(frames_per_packet, total_frames - pos);
            int32_t out_bytes = static_cast<int32_t>(out_buf.size());

            status = encoder.Encode(input_format, output_format,
                                     reinterpret_cast<uint8_t*>(pcm.data() + pos * channels),
                                     out_buf.data(), &out_bytes);

            if (status == ALAC_noErr && out_bytes > 0)
                file.write(reinterpret_cast<char*>(out_buf.data()), out_bytes);
        }

        return file.good();
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".alac" || ext == ".m4a";
    }

    std::string format_name() const override { return "ALAC"; }
};

namespace {
    struct AlacWriterRegistrar {
        AlacWriterRegistrar() {
            FormatRegistry::instance().register_writer(std::make_unique<AlacWriter>());
        }
    };
    static AlacWriterRegistrar alac_writer_registrar;
}

}  // namespace pulp::audio

#endif  // PULP_HAS_ALAC

// AAC writer — lossy audio encoding via Fraunhofer FDK AAC.
// Conditionally compiled: only when fdk-aac is installed (pulp add fdk-aac --accept-license FDK-AAC).
// Auto-registers AacWriter in FormatRegistry via static initializer.

#ifdef PULP_HAS_FDK_AAC

#include <pulp/audio/format_registry.hpp>
#include <fdk-aac/aacenc_lib.h>
#include <vector>
#include <fstream>
#include <cmath>

namespace pulp::audio {

class AacWriter : public FormatWriter {
public:
    bool write(const std::string& path, const AudioFileData& data) override {
        if (data.empty()) return false;

        HANDLE_AACENCODER encoder = nullptr;
        if (aacEncOpen(&encoder, 0, static_cast<UINT>(data.num_channels())) != AACENC_OK)
            return false;

        aacEncoder_SetParam(encoder, AACENC_AOT, 2);  // AAC-LC
        aacEncoder_SetParam(encoder, AACENC_SAMPLERATE, data.sample_rate);
        aacEncoder_SetParam(encoder, AACENC_CHANNELMODE,
                           data.num_channels() == 1 ? MODE_1 : MODE_2);
        aacEncoder_SetParam(encoder, AACENC_BITRATE, 128000);
        aacEncoder_SetParam(encoder, AACENC_TRANSMUX, 2);  // ADTS

        if (aacEncEncode(encoder, nullptr, nullptr, nullptr, nullptr) != AACENC_OK) {
            aacEncClose(&encoder);
            return false;
        }

        std::ofstream file(path, std::ios::binary);
        if (!file) {
            aacEncClose(&encoder);
            return false;
        }

        uint32_t channels = data.num_channels();
        uint32_t total_frames = static_cast<uint32_t>(data.num_frames());

        // Convert to int16 interleaved
        std::vector<INT_PCM> pcm(static_cast<size_t>(total_frames) * channels);
        for (uint32_t f = 0; f < total_frames; ++f)
            for (uint32_t c = 0; c < channels; ++c)
                pcm[f * channels + c] = static_cast<INT_PCM>(
                    std::clamp(data.channels[c][f], -1.0f, 1.0f) * 32767.0f);

        // Encode in frames
        AACENC_InfoStruct info = {};
        aacEncInfo(encoder, &info);
        uint32_t frame_size = info.frameLength;

        std::vector<uint8_t> out_buf(info.maxOutBufBytes);

        for (uint32_t pos = 0; pos < total_frames; pos += frame_size) {
            uint32_t frames_this = std::min(frame_size, total_frames - pos);

            AACENC_BufDesc in_desc = {};
            AACENC_BufDesc out_desc = {};
            AACENC_InArgs in_args = {};
            AACENC_OutArgs out_args = {};

            int in_id = IN_AUDIO_DATA;
            int in_size = static_cast<int>(frames_this * channels * sizeof(INT_PCM));
            int in_elem_size = sizeof(INT_PCM);
            void* in_ptr = pcm.data() + pos * channels;

            in_desc.numBufs = 1;
            in_desc.bufs = &in_ptr;
            in_desc.bufferIdentifiers = &in_id;
            in_desc.bufSizes = &in_size;
            in_desc.bufElSizes = &in_elem_size;

            int out_id = OUT_BITSTREAM_DATA;
            int out_size = static_cast<int>(out_buf.size());
            int out_elem_size = 1;
            void* out_ptr = out_buf.data();

            out_desc.numBufs = 1;
            out_desc.bufs = &out_ptr;
            out_desc.bufferIdentifiers = &out_id;
            out_desc.bufSizes = &out_size;
            out_desc.bufElSizes = &out_elem_size;

            in_args.numInSamples = static_cast<INT>(frames_this * channels);

            if (aacEncEncode(encoder, &in_desc, &out_desc, &in_args, &out_args) == AACENC_OK) {
                if (out_args.numOutBytes > 0)
                    file.write(reinterpret_cast<char*>(out_buf.data()), out_args.numOutBytes);
            }
        }

        aacEncClose(&encoder);
        return file.good();
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".aac" || ext == ".m4a";
    }

    std::string format_name() const override { return "AAC"; }
};

namespace {
    struct AacWriterRegistrar {
        AacWriterRegistrar() {
            FormatRegistry::instance().register_writer(std::make_unique<AacWriter>());
        }
    };
    static AacWriterRegistrar aac_writer_registrar;
}

}  // namespace pulp::audio

#endif  // PULP_HAS_FDK_AAC

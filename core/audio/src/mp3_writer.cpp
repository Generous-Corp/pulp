// MP3 writer — lossy audio encoding via LAME.
// Conditionally compiled: only when LAME is installed (pulp add lame --accept-license LGPL-2.0).
// Auto-registers Mp3Writer in FormatRegistry via static initializer.

#ifdef PULP_HAS_LAME

#include <pulp/audio/format_registry.hpp>
#include <lame/lame.h>
#include <vector>
#include <fstream>
#include <cmath>

namespace pulp::audio {

class Mp3Writer : public FormatWriter {
public:
    bool write(const std::string& path, const AudioFileData& data) override {
        if (data.empty()) return false;

        lame_t lame = lame_init();
        if (!lame) return false;

        uint32_t channels = data.num_channels();
        uint32_t total_frames = static_cast<uint32_t>(data.num_frames());

        lame_set_num_channels(lame, static_cast<int>(channels));
        lame_set_in_samplerate(lame, static_cast<int>(data.sample_rate));
        lame_set_VBR(lame, vbr_default);
        lame_set_quality(lame, 2);  // 0=best, 9=worst

        if (lame_init_params(lame) < 0) {
            lame_close(lame);
            return false;
        }

        std::ofstream file(path, std::ios::binary);
        if (!file) {
            lame_close(lame);
            return false;
        }

        // Convert float to int16
        std::vector<short> left(total_frames), right(total_frames);
        for (uint32_t f = 0; f < total_frames; ++f) {
            left[f] = static_cast<short>(std::clamp(data.channels[0][f], -1.0f, 1.0f) * 32767.0f);
            if (channels > 1)
                right[f] = static_cast<short>(std::clamp(data.channels[1][f], -1.0f, 1.0f) * 32767.0f);
            else
                right[f] = left[f];
        }

        // Encode
        size_t mp3_buf_size = static_cast<size_t>(1.25 * total_frames + 7200);
        std::vector<unsigned char> mp3_buf(mp3_buf_size);

        int written = lame_encode_buffer(lame, left.data(), right.data(),
                                          static_cast<int>(total_frames),
                                          mp3_buf.data(), static_cast<int>(mp3_buf_size));

        if (written > 0)
            file.write(reinterpret_cast<char*>(mp3_buf.data()), written);

        // Flush
        int flushed = lame_encode_flush(lame, mp3_buf.data(), static_cast<int>(mp3_buf_size));
        if (flushed > 0)
            file.write(reinterpret_cast<char*>(mp3_buf.data()), flushed);

        lame_close(lame);
        return file.good();
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".mp3";
    }

    std::string format_name() const override { return "MP3"; }
};

namespace {
    struct Mp3WriterRegistrar {
        Mp3WriterRegistrar() {
            FormatRegistry::instance().register_writer(std::make_unique<Mp3Writer>());
        }
    };
    static Mp3WriterRegistrar mp3_writer_registrar;
}

}  // namespace pulp::audio

#endif  // PULP_HAS_LAME

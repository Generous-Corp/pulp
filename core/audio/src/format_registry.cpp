#include <pulp/audio/format_registry.hpp>
#include <filesystem>
#include <algorithm>
#include <cctype>

// dr_libs for FLAC and MP3 — implementation compiled in codecs.c
#include <dr_flac.h>
#include <dr_mp3.h>

namespace pulp::audio {

// ── Helper ──────────────────────────────────────────────────────────────

static std::string get_extension(const std::string& path) {
    auto ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return ext;
}

// ── WAV Reader/Writer (via CHOC) ────────────────────────────────────────

class WavReader : public FormatReader {
public:
    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        return read_audio_file_info(path);
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        return read_audio_file(path);
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".wav" || ext == ".wave";
    }
    std::string format_name() const override { return "WAV"; }
};

class WavWriter : public FormatWriter {
public:
    bool write(const std::string& path, const AudioFileData& data) override {
        return write_wav_file(path, data);
    }
    bool supports_extension(std::string_view ext) const override {
        return ext == ".wav" || ext == ".wave";
    }
    std::string format_name() const override { return "WAV"; }
};

// ── FLAC Reader (via dr_flac) ───────────────────────────────────────────

class FlacReader : public FormatReader {
public:
    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        drflac* flac = drflac_open_file(path.c_str(), nullptr);
        if (!flac) return std::nullopt;

        AudioFileInfo info;
        info.sample_rate = flac->sampleRate;
        info.num_channels = flac->channels;
        info.num_frames = flac->totalPCMFrameCount;
        info.bits_per_sample = flac->bitsPerSample;
        info.format = "FLAC";
        info.duration_seconds = static_cast<double>(flac->totalPCMFrameCount) / flac->sampleRate;

        drflac_close(flac);
        return info;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        drflac* flac = drflac_open_file(path.c_str(), nullptr);
        if (!flac) return std::nullopt;

        AudioFileData data;
        data.sample_rate = flac->sampleRate;

        uint64_t total_frames = flac->totalPCMFrameCount;
        uint32_t channels = flac->channels;

        // Read as interleaved float
        std::vector<float> interleaved(total_frames * channels);
        uint64_t frames_read = drflac_read_pcm_frames_f32(flac, total_frames, interleaved.data());
        drflac_close(flac);

        if (frames_read == 0) return std::nullopt;

        // Deinterleave
        data.channels.resize(channels);
        for (auto& ch : data.channels)
            ch.resize(static_cast<size_t>(frames_read));

        for (uint64_t i = 0; i < frames_read; ++i)
            for (uint32_t ch = 0; ch < channels; ++ch)
                data.channels[ch][i] = interleaved[i * channels + ch];

        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".flac";
    }
    std::string format_name() const override { return "FLAC"; }
};

// ── MP3 Reader (via dr_mp3) ─────────────────────────────────────────────

class Mp3Reader : public FormatReader {
public:
    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        drmp3 mp3;
        if (!drmp3_init_file(&mp3, path.c_str(), nullptr))
            return std::nullopt;

        AudioFileInfo info;
        info.sample_rate = mp3.sampleRate;
        info.num_channels = mp3.channels;
        info.num_frames = drmp3_get_pcm_frame_count(&mp3);
        info.bits_per_sample = 16;  // MP3 is always decoded to 16-bit equivalent
        info.format = "MP3";
        info.duration_seconds = static_cast<double>(info.num_frames) / info.sample_rate;

        drmp3_uninit(&mp3);
        return info;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        drmp3_config config;
        drmp3_uint64 total_frames;
        float* samples = drmp3_open_file_and_read_pcm_frames_f32(
            path.c_str(), &config, &total_frames, nullptr);

        if (!samples) return std::nullopt;

        AudioFileData data;
        data.sample_rate = config.channels > 0 ? config.sampleRate : 44100;

        uint32_t channels = config.channels;
        data.channels.resize(channels);
        for (auto& ch : data.channels)
            ch.resize(static_cast<size_t>(total_frames));

        // Deinterleave
        for (uint64_t i = 0; i < total_frames; ++i)
            for (uint32_t ch = 0; ch < channels; ++ch)
                data.channels[ch][i] = samples[i * channels + ch];

        drmp3_free(samples, nullptr);
        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".mp3";
    }
    std::string format_name() const override { return "MP3"; }
};

// ── Registry ────────────────────────────────────────────────────────────

FormatRegistry& FormatRegistry::instance() {
    static FormatRegistry registry;
    return registry;
}

FormatRegistry::FormatRegistry() {
    // Register built-in formats
    register_reader(std::make_unique<WavReader>());
    register_writer(std::make_unique<WavWriter>());
    register_reader(std::make_unique<FlacReader>());
    register_reader(std::make_unique<Mp3Reader>());
}

void FormatRegistry::register_reader(std::unique_ptr<FormatReader> reader) {
    readers_.push_back(std::move(reader));
}

void FormatRegistry::register_writer(std::unique_ptr<FormatWriter> writer) {
    writers_.push_back(std::move(writer));
}

FormatReader* FormatRegistry::find_reader(std::string_view extension) const {
    for (auto& r : readers_)
        if (r->supports_extension(extension))
            return r.get();
    return nullptr;
}

FormatWriter* FormatRegistry::find_writer(std::string_view extension) const {
    for (auto& w : writers_)
        if (w->supports_extension(extension))
            return w.get();
    return nullptr;
}

std::optional<AudioFileInfo> FormatRegistry::read_info(const std::string& path) const {
    auto ext = get_extension(path);
    auto* reader = find_reader(ext);
    if (!reader) return std::nullopt;
    return reader->read_info(path);
}

std::optional<AudioFileData> FormatRegistry::read(const std::string& path) const {
    auto ext = get_extension(path);
    auto* reader = find_reader(ext);
    if (!reader) return std::nullopt;
    return reader->read(path);
}

bool FormatRegistry::write(const std::string& path, const AudioFileData& data) const {
    auto ext = get_extension(path);
    auto* writer = find_writer(ext);
    if (!writer) return false;
    return writer->write(path, data);
}

std::vector<std::string> FormatRegistry::supported_read_extensions() const {
    std::vector<std::string> exts;
    for (auto& r : readers_) {
        for (auto& ext : {".wav", ".wave", ".flac", ".mp3", ".ogg"}) {
            if (r->supports_extension(ext))
                exts.push_back(ext);
        }
    }
    return exts;
}

std::vector<std::string> FormatRegistry::supported_write_extensions() const {
    std::vector<std::string> exts;
    for (auto& w : writers_) {
        for (auto& ext : {".wav", ".wave", ".flac", ".mp3", ".ogg"}) {
            if (w->supports_extension(ext))
                exts.push_back(ext);
        }
    }
    return exts;
}

}  // namespace pulp::audio

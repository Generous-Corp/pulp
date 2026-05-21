#include <pulp/audio/format_registry.hpp>
#include <filesystem>
#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

// dr_libs for FLAC and MP3 — implementation compiled in codecs.c
#include <dr_flac.h>
#include <dr_mp3.h>

namespace pulp::audio {

// ── Helper ──────────────────────────────────────────────────────────────

static std::string normalize_extension(std::string_view extension) {
    std::string ext(extension);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (!ext.empty() && ext.front() != '.')
        ext.insert(ext.begin(), '.');
    return ext;
}

static std::string get_extension(const std::string& path) {
    return normalize_extension(std::filesystem::path(path).extension().string());
}

static constexpr std::array<std::string_view, 13> kKnownExtensions = {
    ".wav", ".wave", ".flac", ".mp3", ".ogg", ".oga", ".aiff", ".aif",
    ".aac", ".m4a", ".alac", ".caf", ".aifc"
};

template <typename HandlerList>
static std::vector<std::string> collect_supported_extensions(const HandlerList& handlers) {
    std::vector<std::string> exts;
    for (auto& handler : handlers) {
        for (auto ext : kKnownExtensions) {
            std::string ext_string(ext);
            if (handler->supports_extension(ext) &&
                std::find(exts.begin(), exts.end(), ext_string) == exts.end()) {
                exts.push_back(std::move(ext_string));
            }
        }
    }
    return exts;
}

std::unique_ptr<FormatReader> create_ogg_reader();
std::unique_ptr<FormatReader> create_aiff_reader();
std::unique_ptr<FormatWriter> create_aiff_writer();

#ifdef __APPLE__
std::unique_ptr<FormatReader> create_coreaudio_reader();
#endif

#ifdef PULP_HAS_LIBFLAC
std::unique_ptr<FormatWriter> create_flac_writer();
#endif

#ifdef PULP_HAS_LAME
std::unique_ptr<FormatWriter> create_mp3_writer();
#endif

#ifdef PULP_HAS_FDK_AAC
std::unique_ptr<FormatWriter> create_aac_writer();
#endif

#ifdef PULP_HAS_ALAC
std::unique_ptr<FormatWriter> create_alac_writer();
#endif

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
    register_reader(create_ogg_reader());
    register_reader(create_aiff_reader());
    register_writer(create_aiff_writer());

#ifdef __APPLE__
    register_reader(create_coreaudio_reader());
#endif

#ifdef PULP_HAS_LIBFLAC
    register_writer(create_flac_writer());
#endif

#ifdef PULP_HAS_LAME
    register_writer(create_mp3_writer());
#endif

#ifdef PULP_HAS_FDK_AAC
    register_writer(create_aac_writer());
#endif

#ifdef PULP_HAS_ALAC
    register_writer(create_alac_writer());
#endif
}

void FormatRegistry::register_reader(std::unique_ptr<FormatReader> reader) {
    if (!reader) return;
    readers_.push_back(std::move(reader));
}

void FormatRegistry::register_writer(std::unique_ptr<FormatWriter> writer) {
    if (!writer) return;
    writers_.push_back(std::move(writer));
}

FormatReader* FormatRegistry::find_reader(std::string_view extension) const {
    auto ext = normalize_extension(extension);
    for (auto& r : readers_)
        if (r->supports_extension(ext))
            return r.get();
    return nullptr;
}

FormatWriter* FormatRegistry::find_writer(std::string_view extension) const {
    auto ext = normalize_extension(extension);
    for (auto& w : writers_)
        if (w->supports_extension(ext))
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
    return collect_supported_extensions(readers_);
}

std::vector<std::string> FormatRegistry::supported_write_extensions() const {
    return collect_supported_extensions(writers_);
}

}  // namespace pulp::audio

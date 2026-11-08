#include <pulp/audio/format_registry.hpp>
#include "decode_limits_detail.hpp"
#include "format_capabilities_detail.hpp"
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

std::unique_ptr<FormatWriter> create_system_audio_writer();

#ifdef _WIN32
std::unique_ptr<FormatReader> create_media_foundation_reader();
#endif

// ── WAV Reader/Writer (via CHOC) ────────────────────────────────────────

class WavReader : public FormatReader {
public:
    std::optional<AudioFileInfo> read_info(const std::string& path) override {
        return read_audio_file_info(path);
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        return read(path, DecodeLimits{});
    }

    std::optional<AudioFileData> read(const std::string& path,
                                      const DecodeLimits& limits) override {
        // The header's frame and channel counts are what the decoder sizes
        // its output from; refuse them before it allocates, on the same open
        // that decodes, so a path swapped between two opens cannot present a
        // small header to the check and a large one to the decoder.
        return read_audio_file(path, [&](const AudioFileInfo& info) {
            return detail::within_decode_limits("WAV", info.num_frames, info.num_channels, limits);
        });
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".wav" || ext == ".wave";
    }
    std::string format_name() const override { return "WAV"; }
    FormatCapabilities capabilities() const override {
        return {.format_name = "WAV",
                .extensions = {".wav", ".wave"},
                .bit_depths = {8, 16, 24, 32},
                .float_samples = true,
                .max_channels = 65535};
    }
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
    FormatCapabilities capabilities() const override {
        // write() stores 16-bit integer PCM; write_wav_file() with a
        // WavBitDepth also offers 24-bit and float.
        return {.format_name = "WAV",
                .extensions = {".wav", ".wave"},
                .bit_depths = {16},
                .max_channels = 65535};
    }
};

// ── FLAC Reader (via dr_flac) ───────────────────────────────────────────

// FLAC allows every integer depth from 4 to 32 bits; dr_flac decodes them all.
static std::vector<std::uint32_t> flac_bit_depths() {
    std::vector<std::uint32_t> depths;
    for (std::uint32_t bits = 4; bits <= 32; ++bits)
        depths.push_back(bits);
    return depths;
}

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
        return read(path, DecodeLimits{});
    }

    std::optional<AudioFileData> read(const std::string& path,
                                      const DecodeLimits& limits) override {
        drflac* flac = drflac_open_file(path.c_str(), nullptr);
        if (!flac) return std::nullopt;
        // STREAMINFO's total sample count is a 36-bit untrusted claim, zero
        // when the encoder did not know it; decode_chunked holds it to the
        // limits and to what the stream delivers.
        auto data = detail::decode_chunked("FLAC", flac->sampleRate, flac->channels,
                                           flac->totalPCMFrameCount, limits,
                                           [flac](float* out, uint64_t frames) {
                                               return drflac_read_pcm_frames_f32(flac, frames, out);
                                           });
        drflac_close(flac);
        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".flac";
    }
    std::string format_name() const override { return "FLAC"; }
    FormatCapabilities capabilities() const override {
        return {.format_name = "FLAC",
                .extensions = {".flac"},
                .bit_depths = flac_bit_depths(),
                .max_channels = 8,
                .compressed = true};
    }
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
        // With a Xing/Info header the count is the header's, net of the
        // encoder delay and padding, which is what read() checks the decode
        // against. Without one the stream declares nothing, and dr_mp3's
        // header scan can count a frame the decoder then rejects, so the
        // count is taken by decoding: it is then the count read() delivers.
        info.num_frames =
            mp3.totalPCMFrameCount != DRMP3_UINT64_MAX
                ? drmp3_get_pcm_frame_count(&mp3)
                : detail::count_decoded_frames(mp3.channels, [&mp3](float* out, uint64_t frames) {
                      return drmp3_read_pcm_frames_f32(&mp3, frames, out);
                  });
        info.bits_per_sample = 16;  // MP3 is always decoded to 16-bit equivalent
        info.format = "MP3";
        info.duration_seconds = static_cast<double>(info.num_frames) / info.sample_rate;

        drmp3_uninit(&mp3);
        return info;
    }

    std::optional<AudioFileData> read(const std::string& path) override {
        return read(path, DecodeLimits{});
    }

    std::optional<AudioFileData> read(const std::string& path,
                                      const DecodeLimits& limits) override {
        drmp3 mp3;
        if (!drmp3_init_file(&mp3, path.c_str(), nullptr))
            return std::nullopt;
        // A Xing/Info header declares a frame count, which dr_mp3 reports net
        // of the encoder delay and padding it also trims from the decode;
        // without one the field stays at its maximum and the stream declares
        // nothing. The count comes from the header only, never from a scan.
        const uint64_t claimed_frames =
            mp3.totalPCMFrameCount == DRMP3_UINT64_MAX ? 0u : drmp3_get_pcm_frame_count(&mp3);
        auto data = detail::decode_chunked("MP3", mp3.sampleRate, mp3.channels, claimed_frames,
                                           limits, [&mp3](float* out, uint64_t frames) {
                                               return drmp3_read_pcm_frames_f32(&mp3, frames, out);
                                           });
        drmp3_uninit(&mp3);
        return data;
    }

    bool supports_extension(std::string_view ext) const override {
        return ext == ".mp3";
    }
    std::string format_name() const override { return "MP3"; }
    FormatCapabilities capabilities() const override {
        return {.format_name = "MP3",
                .extensions = {".mp3"},
                .sample_rates = detail::mp3_sample_rates(),
                .max_channels = 2,
                .compressed = true,
                .lossless = false};
    }
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

#ifdef _WIN32
    register_reader(create_media_foundation_reader());
#endif

    // Registered last so an installed optional encoder keeps its extensions.
    // Null (and skipped) where the platform has no system codec.
    register_writer(create_system_audio_writer());
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

std::optional<AudioFileData> FormatRegistry::read(const std::string& path,
                                                  const DecodeLimits& limits) const {
    auto ext = get_extension(path);
    auto* reader = find_reader(ext);
    if (!reader)
        return std::nullopt;
    return reader->read(path, limits);
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

std::vector<FormatCapabilities> FormatRegistry::reader_capabilities() const {
    std::vector<FormatCapabilities> result;
    result.reserve(readers_.size());
    for (const auto& reader : readers_)
        result.push_back(reader->capabilities());
    return result;
}

std::vector<FormatCapabilities> FormatRegistry::writer_capabilities() const {
    std::vector<FormatCapabilities> result;
    result.reserve(writers_.size());
    for (const auto& writer : writers_)
        result.push_back(writer->capabilities());
    return result;
}

std::vector<std::string> FormatRegistry::supported_write_extensions() const {
    return collect_supported_extensions(writers_);
}

}  // namespace pulp::audio

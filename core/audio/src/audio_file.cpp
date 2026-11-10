#include <pulp/audio/audio_file.hpp>
#include "decode_limits_detail.hpp"
#include "pcm_float_scaling.hpp"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
#include <limits>
#include <optional>

// CHOC audio file support
#include <choc/audio/choc_AudioFileFormat_WAV.h>

namespace pulp::audio {

// ── Sample format conversion ─────────────────────────────────────────────────

static float finite_or_zero(float value) {
    return std::isfinite(value) ? value : 0.0f;
}

void int16_to_float(const int16_t* src, float* dst, size_t count) {
    constexpr float scale = 1.0f / 32768.0f;
    for (size_t i = 0; i < count; ++i)
        dst[i] = static_cast<float>(src[i]) * scale;
}

void int24_to_float(const uint8_t* src, float* dst, size_t count) {
    constexpr float scale = 1.0f / 8388608.0f;
    for (size_t i = 0; i < count; ++i) {
        int32_t val = static_cast<int32_t>(src[i * 3])
                    | (static_cast<int32_t>(src[i * 3 + 1]) << 8)
                    | (static_cast<int32_t>(src[i * 3 + 2]) << 16);
        if (val & 0x800000) val |= static_cast<int32_t>(0xFF000000u);
        dst[i] = static_cast<float>(val) * scale;
    }
}

void int32_to_float(const int32_t* src, float* dst, size_t count) {
    constexpr float scale = 1.0f / 2147483648.0f;
    for (size_t i = 0; i < count; ++i)
        dst[i] = static_cast<float>(src[i]) * scale;
}

void float_to_int16(const float* src, int16_t* dst, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        float clamped = std::clamp(finite_or_zero(src[i]), -1.0f, 1.0f);
        dst[i] = static_cast<int16_t>(clamped * 32767.0f);
    }
}

void float_to_int32(const float* src, int32_t* dst, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        float clamped = std::clamp(finite_or_zero(src[i]), -1.0f, 1.0f);
        if (clamped >= 1.0f) {
            dst[i] = std::numeric_limits<int32_t>::max();
        } else if (clamped <= -1.0f) {
            dst[i] = std::numeric_limits<int32_t>::min();
        } else {
            dst[i] = static_cast<int32_t>(static_cast<double>(clamped) * 2147483647.0);
        }
    }
}

// ── Audio file I/O via CHOC ──────────────────────────────────────────────────

static choc::audio::AudioFileFormatList make_format_list() {
    choc::audio::AudioFileFormatList list;
    list.addFormat<choc::audio::WAVAudioFileFormat<true>>();
    return list;
}

static bool has_consistent_channel_lengths(const AudioFileData& data) {
    if (data.empty()) return false;

    const auto expected_frames = data.num_frames();
    return std::all_of(data.channels.begin(), data.channels.end(),
                       [expected_frames](const auto& channel) {
                           return channel.size() == expected_frames;
                       });
}

static bool can_write_wav(const AudioFileData& data) {
    return data.sample_rate != 0 && has_consistent_channel_lengths(data) &&
           data.num_channels() <= std::numeric_limits<uint16_t>::max() &&
           data.num_frames() <= std::numeric_limits<uint32_t>::max();
}

// choc clamps a data chunk that runs past end of file to the bytes present,
// so a header claiming frames the file does not hold also reads as zero
// frames. Only a RIFF/WAVE whose data chunk itself declares zero bytes is a
// genuine zero-frame file; a truncated or lying claim stays a refused decode.
static bool wav_declares_zero_data_bytes(const std::string& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    unsigned char header[12];
    if (!in.read(reinterpret_cast<char*>(header), sizeof(header)))
        return false;
    if (std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0)
        return false;
    for (int chunk = 0; chunk < 64; ++chunk) {
        unsigned char chunk_header[8];
        if (!in.read(reinterpret_cast<char*>(chunk_header), sizeof(chunk_header)))
            return false;
        const uint32_t size = static_cast<uint32_t>(chunk_header[4]) |
                              (static_cast<uint32_t>(chunk_header[5]) << 8) |
                              (static_cast<uint32_t>(chunk_header[6]) << 16) |
                              (static_cast<uint32_t>(chunk_header[7]) << 24);
        if (std::memcmp(chunk_header, "data", 4) == 0)
            return size == 0;
        in.seekg(static_cast<std::streamoff>(size + (size & 1u)), std::ios::cur);
        if (!in)
            return false;
    }
    return false;
}

static std::optional<AudioFileInfo>
info_from_properties(const choc::audio::AudioFileProperties& props) {
    if (!(props.sampleRate > 0) || props.numChannels == 0)
        return std::nullopt;

    AudioFileInfo info;
    info.sample_rate = static_cast<uint32_t>(props.sampleRate);
    info.num_channels = props.numChannels;
    info.num_frames = props.numFrames;
    info.bits_per_sample = choc::audio::getBytesPerSample(props.bitDepth) * 8;
    info.format = props.formatName;
    info.duration_seconds =
        props.numFrames > 0 ? static_cast<double>(props.numFrames) / props.sampleRate : 0;
    return info;
}

std::optional<AudioFileInfo> read_audio_file_info(const std::string& path) {
    try {
        auto formats = make_format_list();
        auto reader = formats.createReader(std::filesystem::path(path));
        if (!reader) return std::nullopt;
        return info_from_properties(reader->getProperties());
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<AudioFileData> read_audio_file(const std::string& path) {
    // The header's frame and channel counts size the allocation below, so a
    // direct caller gets the same default ceilings the registry applies; a
    // WAV declaring 2^30+1 frames is refused here instead of allocating.
    return read_audio_file(path, [](const AudioFileInfo& info) {
        return detail::within_decode_limits("WAV", info.num_frames, info.num_channels,
                                            DecodeLimits{});
    });
}

std::optional<AudioFileData>
read_audio_file(const std::string& path, const std::function<bool(const AudioFileInfo&)>& admit) {
    try {
        auto formats = make_format_list();
        auto reader = formats.createReader(std::filesystem::path(path));
        if (!reader) return std::nullopt;

        auto props = reader->getProperties();
        const auto info = info_from_properties(props);
        if (!info || props.numFrames > std::numeric_limits<uint32_t>::max())
            return std::nullopt;
        // The decode below reads from this same reader, so what `admit` saw
        // is what gets allocated and decoded.
        if (!admit(*info))
            return std::nullopt;

        AudioFileData data;
        data.sample_rate = static_cast<uint32_t>(props.sampleRate);
        data.channels.resize(props.numChannels);

        // A zero-frame file is a valid file with no audio: it decodes to its
        // channel count and rate with empty channels (`AudioFileData::empty()`
        // is true), the same answer libsndfile gives.
        uint32_t frames = static_cast<uint32_t>(props.numFrames);
        if (frames == 0)
            return wav_declares_zero_data_bytes(path)
                       ? std::optional<AudioFileData>(std::move(data))
                       : std::nullopt;

        for (auto& channel : data.channels)
            channel.resize(frames);

        std::vector<float*> planar(props.numChannels);
        for (uint32_t ch = 0; ch < props.numChannels; ++ch)
            planar[ch] = data.channels[ch].data();
        // Pulp's integer scaling (pcm_float_scaling.hpp), shared with the
        // streaming AudioReader so both WAV paths decode bit-identically.
        if (!detail::read_pulp_float(*reader, props.bitDepth, 0, planar.data(), props.numChannels,
                                     frames))
            return std::nullopt;

        return data;
    } catch (...) {
        return std::nullopt;
    }
}

bool write_wav_stream(std::ostream& output, const AudioFileData& data, WavBitDepth bit_depth) {
    if (!can_write_wav(data))
        return false;

    try {
        choc::audio::WAVAudioFileFormat<true> wav;
        choc::audio::AudioFileProperties props;
        props.sampleRate = data.sample_rate;
        props.numChannels = data.num_channels();
        props.bitDepth = bit_depth == WavBitDepth::Float32 ? choc::audio::BitDepth::float32
                       : bit_depth == WavBitDepth::Int24   ? choc::audio::BitDepth::int24
                                                           : choc::audio::BitDepth::int16;

        auto stream = std::shared_ptr<std::ostream>(&output, [](std::ostream*) {});
        auto writer = wav.createWriter(std::move(stream), props);
        if (!writer)
            return false;

        uint32_t frame_count = static_cast<uint32_t>(data.num_frames());
        choc::buffer::ChannelArrayBuffer<float> buffer(data.num_channels(), frame_count);

        for (uint32_t ch = 0; ch < data.num_channels(); ++ch) {
            auto channel_view = buffer.getView().getChannel(ch);
            for (uint32_t i = 0; i < frame_count; ++i)
                channel_view.data.data[i] = data.channels[ch][i];
        }

        const bool appended = writer->appendFrames(buffer.getView());
        const bool flushed = appended && writer->flush();
        writer.reset();
        output.flush();
        return flushed && output.good();
    } catch (...) {
        return false;
    }
}

namespace {

// Flush a closed file's contents to stable storage. False when it cannot be
// opened or the flush fails: a file that may not survive a power loss is not
// published.
bool sync_file_to_disk(const std::filesystem::path& file) {
#if defined(_WIN32)
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    const bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return ok;
#else
    const int fd = ::open(file.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
#endif
}

std::filesystem::path temporary_sibling(const std::filesystem::path& target) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::string suffix;
    for (int i = 0; i < 12; ++i)
        suffix.push_back("0123456789abcdef"[rng() & 0xF]);
    auto directory = target.parent_path();
    if (directory.empty())
        directory = ".";
    return directory / ("." + target.filename().string() + "." + suffix + ".tmp");
}

// Rename without replacing: link() fails if the target exists and is atomic;
// elsewhere an existence check is the best the platform offers.
bool publish_new(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
#if defined(_WIN32)
    if (std::filesystem::exists(to, ec))
        return false;
    std::filesystem::rename(from, to, ec);
    return !ec;
#else
    if (::link(from.c_str(), to.c_str()) != 0)
        return false;
    std::filesystem::remove(from, ec);
    return true;
#endif
}

} // namespace

bool write_file_atomically(const std::string& path, WavReplace replace,
                           const std::function<bool(std::ostream&)>& produce) {
    const std::filesystem::path target(path);
    std::error_code ec;
    if (replace == WavReplace::FailIfExists && std::filesystem::exists(target, ec))
        return false;

    const auto temp = temporary_sibling(target);
    bool published = false;
    try {
        {
            std::ofstream output(temp, std::ios::binary | std::ios::out | std::ios::trunc);
            if (!output)
                return false;
            const bool produced = produce(output);
            output.flush();
            const bool good = produced && output.good();
            output.close();
            if (!good || output.fail()) {
                std::filesystem::remove(temp, ec);
                return false;
            }
        }
        if (!sync_file_to_disk(temp)) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        if (replace == WavReplace::FailIfExists) {
            published = publish_new(temp, target);
        } else {
            std::filesystem::rename(temp, target, ec);
            published = !ec;
        }
    } catch (...) {
        published = false;
    }
    if (!published)
        std::filesystem::remove(temp, ec);
    return published;
}

bool write_wav_file(const std::string& path, const AudioFileData& data, WavBitDepth bit_depth,
                    WavReplace replace) {
    if (!can_write_wav(data))
        return false;
    return write_file_atomically(
        path, replace, [&](std::ostream& out) { return write_wav_stream(out, data, bit_depth); });
}

bool write_wav_file(const std::string& path, const AudioFileData& data, WavBitDepth bit_depth) {
    return write_wav_file(path, data, bit_depth, WavReplace::Replace);
}

bool write_wav_file(const std::string& path, const AudioFileData& data) {
    return write_wav_file(path, data, WavBitDepth::Int16);
}

} // namespace pulp::audio

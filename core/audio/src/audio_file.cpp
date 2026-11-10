#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/audio_reader.hpp>
#include "byte_source_cursor.hpp"
#include "decode_limits_detail.hpp"
#include "wav_frame_reader.hpp"
#include <cmath>
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
#include <memory>
#include <optional>

// CHOC's WAV writer; every WAV read goes through wav_frame_reader.hpp.
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

// ── WAV reading ──────────────────────────────────────────────────────────────

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

namespace {

std::optional<detail::WavSource> open_wav(const std::string& path) {
    detail::WavSource opened;
    if (!opened.open(make_file_byte_source(path)))
        return std::nullopt;
    return opened;
}

std::optional<AudioFileData>
read_wav_admitted(const std::string& path, const std::function<bool(const AudioFileInfo&)>& admit) {
    const auto opened = open_wav(path);
    if (!opened)
        return std::nullopt;
    // The decode below reads through this same open, so what `admit` saw is
    // what gets allocated and decoded.
    if (!admit(detail::wav_file_info(opened->probe)))
        return std::nullopt;
    return detail::decode_all(opened->cursor, opened->probe);
}

} // namespace

std::optional<AudioFileInfo> read_audio_file_info(const std::string& path) {
    const auto opened = open_wav(path);
    if (!opened)
        return std::nullopt;
    return detail::wav_file_info(opened->probe);
}

std::optional<AudioFileData> read_audio_file(const std::string& path) {
    // The recoverable frame and channel counts size the allocation below, so
    // a direct caller gets the same default ceilings the registry applies.
    return read_audio_file(path, [](const AudioFileInfo& info) {
        return detail::within_decode_limits("WAV", info.num_frames, info.num_channels,
                                            DecodeLimits{});
    });
}

std::optional<AudioFileData>
read_audio_file(const std::string& path, const std::function<bool(const AudioFileInfo&)>& admit) {
    // An allocation the caller's admit let through can still fail; a failed
    // read is a refusal, never an exception out of the reader.
    try {
        return read_wav_admitted(path, admit);
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

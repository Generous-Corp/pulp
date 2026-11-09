#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/audio_reader.hpp>
#include "byte_source_cursor.hpp"
#include "decode_limits_detail.hpp"
#include "wav_frame_reader.hpp"
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
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

// Frames decoded per call into the output; bounds the interleaved scratch so
// memory follows the output, not the file.
constexpr uint64_t kReadChunkFrames = 65536u;

// One open of a WAV file: the byte source, the cursor over it, and the
// probe (headers only). The cursor points into the source, which the
// unique_ptr keeps at a stable address.
struct OpenedWav {
    std::unique_ptr<AudioByteSource> source;
    WaveByteCursor cursor;
    detail::WavProbe probe;
};

std::optional<OpenedWav> open_wav(const std::string& path) {
    OpenedWav opened;
    opened.source = make_file_byte_source(path);
    if (!opened.source)
        return std::nullopt;
    opened.cursor = detail::cursor_over(*opened.source);
    opened.probe = detail::probe_wav(opened.cursor);
    if (!opened.probe.accepted())
        return std::nullopt;
    return opened;
}

} // namespace

static std::optional<AudioFileData>
read_wav_admitted(const std::string& path, const std::function<bool(const AudioFileInfo&)>& admit);

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

static std::optional<AudioFileData>
read_wav_admitted(const std::string& path, const std::function<bool(const AudioFileInfo&)>& admit) {
    auto opened = open_wav(path);
    if (!opened)
        return std::nullopt;
    // The decode below reads through this same open, so what `admit` saw is
    // what gets allocated and decoded.
    const auto info = detail::wav_file_info(opened->probe);
    if (!admit(info) || info.num_frames > std::numeric_limits<std::size_t>::max())
        return std::nullopt;

    detail::WavFrameReader reader;
    if (!reader.open(opened->cursor, opened->probe))
        return std::nullopt;

    AudioFileData data;
    data.sample_rate = info.sample_rate;
    data.channels.resize(info.num_channels);
    // A zero-frame file is a valid file with no audio: it decodes to its
    // channel count and rate with empty channels (`AudioFileData::empty()`
    // is true), the same answer libsndfile gives.
    const auto frames = static_cast<std::size_t>(info.num_frames);
    for (auto& channel : data.channels)
        channel.resize(frames);

    const auto channels = static_cast<std::size_t>(info.num_channels);
    std::vector<float> interleaved(
        static_cast<std::size_t>(std::min<uint64_t>(info.num_frames, kReadChunkFrames)) * channels);
    for (uint64_t start = 0; start < info.num_frames; start += kReadChunkFrames) {
        const auto count = std::min<uint64_t>(kReadChunkFrames, info.num_frames - start);
        if (reader.read_interleaved(start, count, interleaved.data()) != count)
            return std::nullopt;
        for (std::size_t ch = 0; ch < channels; ++ch) {
            float* dest = data.channels[ch].data() + start;
            for (std::size_t i = 0; i < static_cast<std::size_t>(count); ++i)
                dest[i] = interleaved[i * channels + ch];
        }
    }
    return data;
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

bool write_wav_file(const std::string& path, const AudioFileData& data, WavBitDepth bit_depth) {
    if (!can_write_wav(data))
        return false;
    std::ofstream output(std::filesystem::path(path),
                         std::ios::binary | std::ios::out | std::ios::trunc);
    if (!output)
        return false;
    const bool written = write_wav_stream(output, data, bit_depth);
    output.close();
    return written && !output.fail();
}

bool write_wav_file(const std::string& path, const AudioFileData& data) {
    return write_wav_file(path, data, WavBitDepth::Int16);
}

} // namespace pulp::audio

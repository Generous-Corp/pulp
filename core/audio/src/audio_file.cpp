#include <pulp/audio/audio_file.hpp>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
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

// Integer PCM decodes to float by dividing by 2^(bits-1): int16 -32768 maps to
// -1.0f and +32767 to 32767/32768. That is the convention of int16_to_float
// above, sample_converter.hpp, the AIFF reader, dr_wav and libsndfile. CHOC's
// readFrames divides by (2^(bits-1) - 1) instead, so the integer depths are
// read at double precision, the original code is recovered exactly (a double
// carries more than twice the bits of any PCM depth, so rounding the product
// lands on the code), and rescaled here. Float files are read as they are.
struct IntegerDepth {
    double choc_full_scale; // what CHOC divided the code by
    float scale;            // 1 / 2^(bits-1)
};

static std::optional<IntegerDepth> integer_depth(choc::audio::BitDepth depth) {
    using choc::audio::BitDepth;
    switch (depth) {
    case BitDepth::int8:
        return IntegerDepth{127.0, 1.0f / 128.0f};
    case BitDepth::int16:
        return IntegerDepth{32767.0, 1.0f / 32768.0f};
    case BitDepth::int24:
        return IntegerDepth{8388607.0, 1.0f / 8388608.0f};
    case BitDepth::int32:
        return IntegerDepth{2147483647.0, 1.0f / 2147483648.0f};
    default:
        return std::nullopt;
    }
}

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
    return read_audio_file(path, [](const AudioFileInfo&) { return true; });
}

std::optional<AudioFileData>
read_audio_file(const std::string& path, const std::function<bool(const AudioFileInfo&)>& admit) {
    try {
        auto formats = make_format_list();
        auto reader = formats.createReader(std::filesystem::path(path));
        if (!reader) return std::nullopt;

        auto props = reader->getProperties();
        const auto info = info_from_properties(props);
        if (!info || props.numFrames == 0 || props.numFrames > std::numeric_limits<uint32_t>::max())
            return std::nullopt;
        // The decode below reads from this same reader, so what `admit` saw
        // is what gets allocated and decoded.
        if (!admit(*info))
            return std::nullopt;

        uint32_t frames = static_cast<uint32_t>(props.numFrames);

        AudioFileData data;
        data.sample_rate = static_cast<uint32_t>(props.sampleRate);
        data.channels.resize(props.numChannels);
        for (auto& channel : data.channels)
            channel.resize(frames);

        if (const auto depth = integer_depth(props.bitDepth)) {
            // Chunked so the double-precision scratch stays a bounded
            // transient rather than doubling the admitted output size.
            constexpr uint32_t kChunkFrames = 65536u;
            choc::buffer::ChannelArrayBuffer<double> chunk(props.numChannels,
                                                           std::min(frames, kChunkFrames));
            for (uint32_t start = 0; start < frames; start += kChunkFrames) {
                const uint32_t count = std::min(kChunkFrames, frames - start);
                auto view = chunk.getView().getStart(count);
                if (!reader->readFrames(start, view))
                    return std::nullopt;
                for (uint32_t ch = 0; ch < props.numChannels; ++ch) {
                    const double* scaled = view.getChannel(ch).data.data;
                    float* out = data.channels[ch].data() + start;
                    for (uint32_t i = 0; i < count; ++i) {
                        const auto code = std::llround(scaled[i] * depth->choc_full_scale);
                        out[i] = static_cast<float>(code) * depth->scale;
                    }
                }
            }
            return data;
        }

        choc::buffer::ChannelArrayBuffer<float> buffer(props.numChannels, frames);
        if (!reader->readFrames(0, buffer.getView()))
            return std::nullopt;

        for (uint32_t ch = 0; ch < props.numChannels; ++ch) {
            auto channel_view = buffer.getView().getChannel(ch);
            for (uint32_t i = 0; i < frames; ++i)
                data.channels[ch][i] = channel_view.data.data[i];
        }

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

#include <pulp/audio/audio_file.hpp>
#include <cmath>
#include <algorithm>
#include <filesystem>

// CHOC audio file support
#include <choc/audio/choc_AudioFileFormat_WAV.h>

namespace pulp::audio {

// ── Sample format conversion ─────────────────────────────────────────────────

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
        float clamped = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<int16_t>(clamped * 32767.0f);
    }
}

void float_to_int32(const float* src, int32_t* dst, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        float clamped = std::clamp(src[i], -1.0f, 1.0f);
        dst[i] = static_cast<int32_t>(clamped * 2147483647.0f);
    }
}

// ── Audio file I/O via CHOC ──────────────────────────────────────────────────

static choc::audio::AudioFileFormatList make_format_list() {
    choc::audio::AudioFileFormatList list;
    list.addFormat<choc::audio::WAVAudioFileFormat<true>>();
    return list;
}

std::optional<AudioFileInfo> read_audio_file_info(const std::string& path) {
    try {
        auto formats = make_format_list();
        auto reader = formats.createReader(std::filesystem::path(path));
        if (!reader) return std::nullopt;

        auto props = reader->getProperties();
        AudioFileInfo info;
        info.sample_rate = static_cast<uint32_t>(props.sampleRate);
        info.num_channels = props.numChannels;
        info.num_frames = props.numFrames;
        info.bits_per_sample = choc::audio::getBytesPerSample(props.bitDepth) * 8;
        info.format = props.formatName;
        info.duration_seconds = props.numFrames > 0
            ? static_cast<double>(props.numFrames) / props.sampleRate : 0;
        return info;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<AudioFileData> read_audio_file(const std::string& path) {
    try {
        auto formats = make_format_list();
        auto reader = formats.createReader(std::filesystem::path(path));
        if (!reader) return std::nullopt;

        auto props = reader->getProperties();
        if (props.numFrames == 0 || props.numChannels == 0) return std::nullopt;

        uint32_t frames = static_cast<uint32_t>(props.numFrames);

        choc::buffer::ChannelArrayBuffer<float> buffer(props.numChannels, frames);
        if (!reader->readFrames(0, buffer.getView()))
            return std::nullopt;

        AudioFileData data;
        data.sample_rate = static_cast<uint32_t>(props.sampleRate);
        data.channels.resize(props.numChannels);

        for (uint32_t ch = 0; ch < props.numChannels; ++ch) {
            data.channels[ch].resize(frames);
            auto channel_view = buffer.getView().getChannel(ch);
            for (uint32_t i = 0; i < frames; ++i)
                data.channels[ch][i] = channel_view.data.data[i];
        }

        return data;
    } catch (...) {
        return std::nullopt;
    }
}

bool write_wav_file(const std::string& path, const AudioFileData& data) {
    if (data.empty()) return false;

    try {
        choc::audio::WAVAudioFileFormat<true> wav;
        choc::audio::AudioFileProperties props;
        props.sampleRate = data.sample_rate;
        props.numChannels = data.num_channels();
        props.bitDepth = choc::audio::BitDepth::int16;

        auto writer = wav.createWriter(std::filesystem::path(path), props);
        if (!writer) return false;

        uint32_t frames = static_cast<uint32_t>(data.num_frames());
        choc::buffer::ChannelArrayBuffer<float> buffer(data.num_channels(), frames);

        for (uint32_t ch = 0; ch < data.num_channels(); ++ch) {
            auto channel_view = buffer.getView().getChannel(ch);
            for (uint32_t i = 0; i < frames; ++i)
                channel_view.data.data[i] = data.channels[ch][i];
        }

        return writer->appendFrames(buffer.getView());
    } catch (...) {
        return false;
    }
}

} // namespace pulp::audio

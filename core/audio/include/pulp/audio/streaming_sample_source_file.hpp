#pragma once

/// @file streaming_sample_source_file.hpp
/// File-backed FrameReader factory for StreamingSampleSource.
///
/// Adapts an audio file into a FrameReader so StreamingSampleSource can play it
/// through the streaming machinery (resident preload + background-filled ring +
/// RT-safe pull) — the audio thread never decodes or allocates.
///
/// Current limitation (honest): the file is decoded ONCE, off the audio thread,
/// into a resident PCM buffer, and the reader then serves frame ranges from that
/// buffer. It is therefore not yet a "too large to hold resident" path — true
/// ranged / zero-copy disk streaming awaits ranged-decode support in
/// MemoryMappedAudioReader (today its read_frames decodes the whole file per
/// call). The StreamingSampleSource primitive itself is codec-agnostic: any
/// FrameReader that range-reads from disk gets true streaming with no change
/// here. Header-only; composes existing pulp::audio APIs.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/mmap_reader.hpp>
#include <pulp/audio/streaming_sample_source.hpp>

namespace pulp::audio {

/// A FrameReader plus the opened file's properties, ready to feed into a
/// StreamingSampleSourceConfig (channels / total_frames / sample_rate).
struct FileFrameReader {
    FrameReader reader;
    std::uint32_t channels = 0;
    std::uint64_t total_frames = 0;
    std::uint32_t sample_rate = 0;
    bool valid = false;
};

/// Open + decode @p path once (off the audio thread) and return a FrameReader
/// that serves frame ranges from the resident decode. On failure returns a
/// FileFrameReader with valid == false. Control thread only.
inline FileFrameReader make_memory_mapped_frame_reader(std::string_view path) {
    FileFrameReader result;

    MemoryMappedAudioReader mmap;
    if (!mmap.open(path)) {
        return result;  // valid == false
    }
    auto decoded = std::make_shared<AudioFileData>();
    if (auto data = mmap.read_all()) {
        *decoded = std::move(*data);
    } else {
        return result;
    }
    mmap.close();  // the resident decode owns the PCM now

    const std::uint32_t channels = decoded->num_channels();
    const std::uint64_t total = decoded->num_frames();
    if (channels == 0 || total == 0) {
        return result;  // unusable file
    }

    result.channels = channels;
    result.total_frames = total;
    result.sample_rate = decoded->sample_rate;
    result.reader = [decoded, channels, total](std::uint64_t start_frame,
                                               BufferView<float> dest,
                                               std::uint64_t frames)
        -> std::uint64_t {
        if (start_frame >= total) return 0;
        const std::uint64_t n = std::min(frames, total - start_frame);
        if (n == 0) return 0;
        // Fill only the channels the destination actually has — never write
        // through a missing channel. Extra source channels are dropped; extra
        // dest channels are left as-is (the source clears unfilled channels).
        const std::uint32_t use_ch = std::min<std::uint32_t>(
            channels, static_cast<std::uint32_t>(dest.num_channels()));
        for (std::uint32_t ch = 0; ch < use_ch; ++ch) {
            const float* src = decoded->channels[ch].data() +
                               static_cast<std::size_t>(start_frame);
            std::copy_n(src, static_cast<std::size_t>(n), dest.channel_ptr(ch));
        }
        return n;
    };
    result.valid = true;
    return result;
}

}  // namespace pulp::audio

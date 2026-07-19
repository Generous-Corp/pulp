#pragma once

/// @file streaming_sample_source_file.hpp
/// File-backed FrameReader factory for StreamingSampleSource.
///
/// Adapts an audio file into a FrameReader so StreamingSampleSource can play it
/// through the streaming machinery (resident preload + background-filled ring +
/// RT-safe pull) — the audio thread never decodes or allocates.
///
/// The returned callback retains one MemoryMappedAudioReader and delegates each
/// request to read_frames(). Seek-readable formats such as WAV therefore decode
/// only the requested range. Formats without a ranged decoder use the reader's
/// explicit decode-once fallback; callers can inspect supports_ranged_read before
/// admitting a source under a strict streaming-memory policy.

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/mmap_reader.hpp>
#include <pulp/audio/streaming_sample_source.hpp>
#include <pulp/runtime/crypto.hpp>

namespace pulp::audio {

/// A FrameReader plus the opened file's properties, ready to feed into a
/// StreamingSampleSourceConfig (channels / total_frames / sample_rate).
struct FileFrameReader {
    FrameReader reader;
    FrameReaderBinding binding;
    std::uint32_t channels = 0;
    std::uint64_t total_frames = 0;
    std::uint32_t sample_rate = 0;
    std::array<std::uint8_t, 32> content_sha256{};
    std::uint64_t mapped_byte_size = 0;
    bool has_content_identity = false;
    bool supports_ranged_read = false;
    bool valid = false;
};

/// Bind an existing immutable mapped snapshot. This can tighten a capability
/// probe to ranged-only reads without copying the source file a second time.
inline FileFrameReader make_retained_memory_mapped_frame_reader(
    std::shared_ptr<MemoryMappedAudioReader> mapped,
    bool require_ranged_read = false,
    bool compute_content_identity = false,
    std::uint64_t maximum_mapped_bytes = std::numeric_limits<std::uint64_t>::max()) {
    FileFrameReader result;
    if (!mapped || !mapped->is_open()) {
        return result; // valid == false
    }
    if (mapped->size() > maximum_mapped_bytes) {
        return result;
    }

    const auto& info = mapped->info();
    const std::uint32_t channels = info.num_channels;
    const std::uint64_t total = info.num_frames;
    if (channels == 0 || total == 0) {
        return result; // unusable file
    }

    result.channels = channels;
    result.total_frames = total;
    result.sample_rate = info.sample_rate;
    if (compute_content_identity) {
        const auto digest = runtime::sha256(mapped->data(), mapped->size());
        if (digest.size() != result.content_sha256.size())
            return {};
        std::copy(digest.begin(), digest.end(), result.content_sha256.begin());
        result.mapped_byte_size = mapped->size();
        result.has_content_identity = true;
    }
    result.supports_ranged_read = mapped->supports_ranged_read();
    if (require_ranged_read && !result.supports_ranged_read)
        return result;
    result.binding.stop_mode = result.supports_ranged_read ? FrameReaderStopMode::Cooperative
                                                           : FrameReaderStopMode::JoinOnly;
    result.binding.read = [mapped = std::move(mapped), channels, total, require_ranged_read,
                           destinations = std::vector<float*>(channels)](
                              std::uint64_t start_frame, BufferView<float> dest,
                              std::uint64_t frames,
                              std::stop_token stop_token) mutable -> std::uint64_t {
        constexpr std::uint64_t kStopCheckFrames = 16384;
        if (start_frame >= total)
            return 0;
        const std::uint64_t n = std::min({
            frames,
            total - start_frame,
            static_cast<std::uint64_t>(dest.num_samples()),
        });
        if (n == 0)
            return 0;
        const std::uint32_t use_ch =
            std::min<std::uint32_t>(channels, static_cast<std::uint32_t>(dest.num_channels()));
        if (use_ch == 0)
            return 0;

        std::uint64_t produced = 0;
        while (produced < n && !stop_token.stop_requested()) {
            const auto chunk = std::min(kStopCheckFrames, n - produced);
            for (std::uint32_t ch = 0; ch < use_ch; ++ch) {
                destinations[ch] = dest.channel_ptr(ch) + produced;
            }
            const auto read = require_ranged_read
                                  ? mapped->read_frames_ranged_only(destinations.data(), use_ch,

                                                                    start_frame + produced, chunk)
                                  : mapped->read_frames(destinations.data(), use_ch,
                                                        start_frame + produced, chunk);
            if (!read) {
                return produced;
            }
            produced += chunk;
        }
        return produced;
    };
    const auto stoppable = result.binding.read;
    result.reader = [stoppable](std::uint64_t start_frame, BufferView<float> destination,
                                std::uint64_t frames) mutable {
        return stoppable(start_frame, destination, frames, {});
    };
    result.valid = true;
    return result;
}

/// Open and retain a mapped reader for @p path. With @p require_ranged_read,
/// unsupported formats are rejected and a later ranged decode failure is
/// reported rather than falling back to a whole-file decode. On failure returns
/// a FileFrameReader with valid == false. Control thread only.
inline FileFrameReader make_memory_mapped_frame_reader(
    std::string_view path, bool require_ranged_read = false, bool compute_content_identity = false,
    std::uint64_t maximum_mapped_bytes = std::numeric_limits<std::uint64_t>::max(),
    std::shared_ptr<MemoryMappedAudioReader>* retained_reader = nullptr) {
    if (retained_reader != nullptr)
        retained_reader->reset();
    auto mapped = std::make_shared<MemoryMappedAudioReader>();
    const auto mapped_limit = static_cast<std::size_t>(
        std::min<std::uint64_t>(maximum_mapped_bytes, std::numeric_limits<std::size_t>::max()));
    if (!mapped->open(path, mapped_limit))
        return {};
    auto retained = mapped;
    auto result = make_retained_memory_mapped_frame_reader(
        std::move(mapped), require_ranged_read, compute_content_identity,
        maximum_mapped_bytes);
    if (result.valid && retained_reader != nullptr)
        *retained_reader = std::move(retained);
    return result;
}

} // namespace pulp::audio

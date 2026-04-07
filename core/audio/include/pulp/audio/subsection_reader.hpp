#pragma once

// AudioSubsectionReader — reads a sub-range of frames from audio file data.
// Wraps AudioFileData and provides a view into a portion of the file.

#include <pulp/audio/audio_file.hpp>
#include <algorithm>
#include <cstdint>

namespace pulp::audio {

/// A view into a sub-range of an AudioFileData.
/// Does not copy the audio data — references the original.
class AudioSubsectionReader {
public:
    AudioSubsectionReader() = default;

    /// Create a subsection from frame start_frame to start_frame + length_frames.
    AudioSubsectionReader(const AudioFileData& source, uint64_t start_frame, uint64_t length_frames)
        : source_(&source)
        , start_(std::min(start_frame, source.num_frames()))
        , length_(std::min(length_frames, source.num_frames() - start_)) {}

    /// Number of frames in this subsection
    uint64_t num_frames() const { return length_; }

    /// Number of channels
    uint32_t num_channels() const { return source_ ? source_->num_channels() : 0; }

    /// Sample rate (inherited from source)
    uint32_t sample_rate() const { return source_ ? source_->sample_rate : 0; }

    /// Read a frame from this subsection
    float sample(uint32_t channel, uint64_t frame) const {
        if (!source_ || channel >= num_channels() || frame >= length_) return 0.0f;
        return source_->channels[channel][static_cast<size_t>(start_ + frame)];
    }

    /// Copy frames from this subsection into a destination buffer
    void read_frames(float* dest, uint32_t channel, uint64_t start, uint64_t count) const {
        if (!source_ || channel >= num_channels()) return;
        uint64_t actual_start = start_ + std::min(start, length_);
        uint64_t actual_count = std::min(count, length_ - std::min(start, length_));

        auto& ch = source_->channels[channel];
        for (uint64_t i = 0; i < actual_count; ++i)
            dest[i] = ch[static_cast<size_t>(actual_start + i)];
    }

    /// Extract this subsection as a new AudioFileData (copies data)
    AudioFileData extract() const {
        AudioFileData result;
        if (!source_) return result;

        result.sample_rate = source_->sample_rate;
        result.channels.resize(num_channels());
        for (uint32_t ch = 0; ch < num_channels(); ++ch) {
            result.channels[ch].resize(static_cast<size_t>(length_));
            for (uint64_t i = 0; i < length_; ++i)
                result.channels[ch][static_cast<size_t>(i)] =
                    source_->channels[ch][static_cast<size_t>(start_ + i)];
        }
        return result;
    }

    /// Duration in seconds
    double duration_seconds() const {
        return sample_rate() > 0 ? static_cast<double>(length_) / sample_rate() : 0;
    }

    /// Whether this reader has a valid source
    bool is_valid() const { return source_ != nullptr && length_ > 0; }

private:
    const AudioFileData* source_ = nullptr;
    uint64_t start_ = 0;
    uint64_t length_ = 0;
};

}  // namespace pulp::audio

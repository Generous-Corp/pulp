#pragma once

/// @file format_reader_source.hpp
/// `AudioFormatReaderSource` — wraps a `MemoryMappedAudioReader` as
/// a seekable `PositionableAudioSource`.

#include <pulp/audio/mmap_reader.hpp>
#include <pulp/audio/source.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::audio {

/// Plays back a `MemoryMappedAudioReader` as a `PositionableAudioSource`.
/// Read position is tracked in frames from the start of the file.
/// Loops back to 0 when `set_looping(true)` is set and the read head
/// reaches the end. Past-end reads on non-looping sources fill the
/// remainder of the requested block with silence.
class AudioFormatReaderSource : public PositionableAudioSource {
public:
    AudioFormatReaderSource() = default;

    explicit AudioFormatReaderSource(std::shared_ptr<MemoryMappedAudioReader> reader)
        : reader_(std::move(reader)) {
        reserve_channel_scratch();
    }

    void set_reader(std::shared_ptr<MemoryMappedAudioReader> reader) {
        reader_ = std::move(reader);
        read_pos_ = 0;
        reserve_channel_scratch();
    }

    /// Non-owning accessor — useful when the caller already owns the
    /// reader and wants to query info / metadata.
    MemoryMappedAudioReader* reader() const { return reader_.get(); }

    // ── AudioSource ────────────────────────────────────────────────────────

    void prepare_to_play(int /*samples_per_block*/, double /*sample_rate*/) override {
        // Pre-size the channel-pointer scratch so get_next_audio_block() — an
        // audio-thread call per the AudioSource contract — never allocates.
        reserve_channel_scratch();
    }

    void release_resources() override {
        // Reader lifetime is owned by the shared_ptr.
    }

    void get_next_audio_block(BufferView<float> out,
                               int start_sample,
                               int num_samples) override {
        if (num_samples <= 0 || out.num_channels() == 0) return;
        const auto num_channels = static_cast<uint32_t>(out.num_channels());
        // Reuse the persistent channel-pointer scratch (sized in
        // prepare_to_play()); grow only if a caller exceeds the prepared count,
        // so the steady-state audio-thread path allocates nothing.
        if (channel_scratch_.size() < num_channels)
            channel_scratch_.resize(num_channels);
        if (!reader_) {
            zero_fill(out, num_channels, start_sample, num_samples);
            return;
        }
        int frames_filled = 0;
        while (frames_filled < num_samples) {
            const int remaining = num_samples - frames_filled;
            const uint64_t length = get_total_length();
            uint64_t available = 0;
            if (length > read_pos_) available = length - read_pos_;
            const int chunk = (length == 0)
                ? remaining
                : std::min<int>(remaining, static_cast<int>(std::min<uint64_t>(
                                              available,
                                              static_cast<uint64_t>(remaining))));
            if (chunk <= 0) {
                if (looping_ && length > 0) {
                    read_pos_ = 0;
                    continue;
                }
                // No more data and not looping → silence the rest.
                zero_fill(out, num_channels,
                          start_sample + frames_filled, num_samples - frames_filled);
                return;
            }
            // Point the persistent scratch at this chunk's destination span.
            float** chunk_ptrs = channel_scratch_.data();
            for (uint32_t c = 0; c < num_channels; ++c) {
                chunk_ptrs[c] = out.channel_ptr(c) + start_sample + frames_filled;
            }
            const bool ok = reader_->read_frames(chunk_ptrs,
                                                  num_channels,
                                                  read_pos_,
                                                  static_cast<uint64_t>(chunk));
            if (!ok) {
                zero_fill(out, num_channels,
                          start_sample + frames_filled, num_samples - frames_filled);
                return;
            }
            read_pos_ += static_cast<uint64_t>(chunk);
            frames_filled += chunk;
            if (looping_ && length > 0 && read_pos_ >= length) {
                read_pos_ = 0;
            }
        }
    }

    // ── PositionableAudioSource ────────────────────────────────────────────

    void set_next_read_position(uint64_t new_position) override {
        const uint64_t length = get_total_length();
        if (length > 0 && new_position > length) new_position = length;
        read_pos_ = new_position;
    }

    uint64_t get_next_read_position() const override { return read_pos_; }

    uint64_t get_total_length() const override {
        if (!reader_) return 0;
        return reader_->info().num_frames;
    }

    bool is_looping() const override { return looping_; }
    void set_looping(bool should_loop) override { looping_ = should_loop; }

private:
    // Grow (never shrink) the channel-pointer scratch to the reader's channel
    // count, so the audio-thread block path can reuse it without allocating.
    void reserve_channel_scratch() {
        if (!reader_) return;
        const auto needed = static_cast<std::size_t>(reader_->info().num_channels);
        if (channel_scratch_.size() < needed) channel_scratch_.resize(needed);
    }

    static void zero_fill(BufferView<float>& out, uint32_t num_channels,
                           int start, int count) {
        for (uint32_t c = 0; c < num_channels; ++c) {
            float* p = out.channel_ptr(c) + start;
            for (int i = 0; i < count; ++i) p[i] = 0.0f;
        }
    }

    std::shared_ptr<MemoryMappedAudioReader> reader_;
    // Persistent per-channel pointer scratch — reused across blocks so the
    // audio thread does not allocate. Sized in prepare_to_play(); grows only.
    std::vector<float*> channel_scratch_;
    uint64_t read_pos_ = 0;
    bool looping_ = false;
};

} // namespace pulp::audio

#pragma once

/// @file buffering_reader.hpp
/// Background-thread pre-buffering for audio file playback.

#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <cstring>
#include <algorithm>
#include <limits>

namespace pulp::audio {

/// Background-thread audio file reader for gapless streaming playback.
///
/// Maintains a ring buffer that is filled from a background thread while
/// the audio thread reads from it. The audio thread never blocks on I/O.
///
/// @code
/// BufferingReader reader;
/// reader.set_read_callback([&](float* dest, int frames, int channels) {
///     return audio_file.read(dest, frames, channels);
/// });
/// reader.start(2, 44100); // 2 channels, 1 second buffer
///
/// // In audio callback:
/// int got = reader.read(output_buffer, num_frames, num_channels);
/// @endcode
class BufferingReader {
public:
    /// Callback that reads audio data from a source (file, network, etc.).
    /// @param dest Destination interleaved buffer.
    /// @param num_frames Number of frames to read.
    /// @param num_channels Number of channels.
    /// @return Number of frames actually read (0 = end of source).
    using ReadCallback = std::function<int(float* dest, int num_frames, int num_channels)>;

    BufferingReader() = default;

    ~BufferingReader() {
        stop();
    }

    BufferingReader(const BufferingReader&) = delete;
    BufferingReader& operator=(const BufferingReader&) = delete;

    /// Set the callback that provides audio data.
    void set_read_callback(ReadCallback cb) { read_callback_ = std::move(cb); }

    /// Start the background reader thread.
    /// @param num_channels Number of audio channels.
    /// @param buffer_frames Size of the ring buffer in frames (default: 1 second at 44.1kHz).
    void start(int num_channels, int buffer_frames = 44100) {
        stop();

        if (num_channels <= 0 || buffer_frames <= 0 ||
            num_channels > std::numeric_limits<int>::max() / buffer_frames ||
            num_channels > std::numeric_limits<int>::max() / kChunkFrames) {
            num_channels_ = 0;
            buffer_size_ = 0;
            write_pos_ = 0;
            read_pos_ = 0;
            available_.store(0);
            finished_.store(false);
            running_.store(false);
            return;
        }

        const auto sample_capacity =
            static_cast<int64_t>(buffer_frames) * static_cast<int64_t>(num_channels);
        num_channels_ = num_channels;
        buffer_size_ = static_cast<int>(sample_capacity);
        buffer_.resize(static_cast<size_t>(buffer_size_), 0.0f);
        write_pos_ = 0;
        read_pos_ = 0;
        available_.store(0);
        finished_.store(false);
        running_.store(true);

        thread_ = std::thread([this] { background_loop(); });
    }

    /// Stop the background thread and release resources.
    void stop() {
        running_.store(false);
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        buffer_.clear();
    }

    /// Read frames from the buffer (audio thread, lock-free on the hot path).
    /// @param dest Destination interleaved buffer.
    /// @param num_frames Number of frames requested.
    /// @param num_channels Number of channels (must match start()).
    /// @return Number of frames actually copied (may be less if buffer underrun).
    int read(float* dest, int num_frames, int num_channels) {
        if (dest == nullptr || num_frames <= 0) return 0;
        if (num_channels != num_channels_ || buffer_.empty()) return 0;
        if (num_channels <= 0 ||
            num_frames > std::numeric_limits<int>::max() / num_channels) return 0;

        int samples_requested = num_frames * num_channels;
        int avail = available_.load(std::memory_order_acquire);
        int samples_to_read = std::min(samples_requested, avail);

        if (samples_to_read <= 0) {
            std::memset(dest, 0, static_cast<size_t>(samples_requested) * sizeof(float));
            return 0;
        }

        // Copy from ring buffer
        int first_chunk = std::min(samples_to_read, buffer_size_ - read_pos_);
        std::memcpy(dest, buffer_.data() + read_pos_,
                     static_cast<size_t>(first_chunk) * sizeof(float));

        int second_chunk = samples_to_read - first_chunk;
        if (second_chunk > 0) {
            std::memcpy(dest + first_chunk, buffer_.data(),
                         static_cast<size_t>(second_chunk) * sizeof(float));
        }

        read_pos_ = (read_pos_ + samples_to_read) % buffer_size_;
        available_.fetch_sub(samples_to_read, std::memory_order_release);

        // Wake up background thread if it's waiting for space
        cv_.notify_one();

        // Zero-fill any remaining samples
        int frames_read = samples_to_read / num_channels;
        if (frames_read < num_frames) {
            std::memset(dest + samples_to_read, 0,
                         static_cast<size_t>((num_frames - frames_read) * num_channels) * sizeof(float));
        }

        return frames_read;
    }

    /// Check if the source has been fully read.
    bool is_finished() const { return finished_.load(); }

    /// Check if the reader is running.
    bool is_running() const { return running_.load(); }

    /// Number of frames currently available in the buffer.
    int frames_available() const {
        return num_channels_ > 0 ? available_.load() / num_channels_ : 0;
    }

private:
    ReadCallback read_callback_;
    std::vector<float> buffer_;
    int buffer_size_ = 0;
    int num_channels_ = 0;
    int write_pos_ = 0;
    int read_pos_ = 0;
    std::atomic<int> available_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    static constexpr int kChunkFrames = 1024;

    void background_loop() {
        std::vector<float> temp(static_cast<size_t>(kChunkFrames * num_channels_));

        while (running_.load()) {
            // Wait until there's space in the buffer
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                    return !running_.load() ||
                           available_.load() < buffer_size_;
                });
            }

            if (!running_.load()) break;

            int space = buffer_size_ - available_.load(std::memory_order_acquire);
            int frames_to_read = std::min(kChunkFrames, space / std::max(1, num_channels_));

            if (frames_to_read <= 0) continue;

            int frames_got = 0;
            if (read_callback_) {
                frames_got = read_callback_(temp.data(), frames_to_read, num_channels_);
            }

            if (frames_got <= 0) {
                finished_.store(true);
                running_.store(false);
                cv_.notify_all();
                return;
            }
            if (frames_got > frames_to_read) frames_got = frames_to_read;

            int samples = frames_got * num_channels_;
            int first_chunk = std::min(samples, buffer_size_ - write_pos_);
            std::memcpy(buffer_.data() + write_pos_, temp.data(),
                         static_cast<size_t>(first_chunk) * sizeof(float));

            int second_chunk = samples - first_chunk;
            if (second_chunk > 0) {
                std::memcpy(buffer_.data(), temp.data() + first_chunk,
                             static_cast<size_t>(second_chunk) * sizeof(float));
            }

            write_pos_ = (write_pos_ + samples) % buffer_size_;
            available_.fetch_add(samples, std::memory_order_release);
        }
    }
};

} // namespace pulp::audio

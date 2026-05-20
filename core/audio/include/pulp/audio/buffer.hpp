#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace pulp::audio {

/// Non-owning view over multi-channel audio sample data.
///
/// This is the primary type passed into Processor::process(). It wraps an
/// array of channel pointers provided by the host or format adapter and does
/// not allocate or own any memory.
///
/// @tparam SampleType  Sample format — typically @c float or @c const @c float.
///
/// @code
/// // Wrap host-provided pointers:
/// float* ptrs[2] = {left_ptr, right_ptr};
/// pulp::audio::BufferView<float> buf(ptrs, 2, 512);
///
/// // Process samples:
/// for (auto& s : buf.channel(0)) s *= 0.5f;
/// @endcode
template<typename SampleType = float>
class BufferView {
public:
    BufferView() = default;

    /// Construct from an array of per-channel pointers.
    /// @param channel_ptrs  Array of at least @p num_channels pointers.
    /// @param num_channels  Number of audio channels.
    /// @param num_samples   Number of samples per channel in this block.
    BufferView(SampleType* const* channel_ptrs, std::size_t num_channels, std::size_t num_samples)
        : channels_(channel_ptrs)
        , num_channels_(num_channels)
        , num_samples_(num_samples)
    {}

    /// Create a non-owning slice over this view.
    ///
    /// The returned view references the same channel-pointer array and is
    /// valid for no longer than the source view. @p start and @p length are
    /// clamped to this view's sample range.
    BufferView slice(std::size_t start, std::size_t length) const {
        if (start > num_samples_) start = num_samples_;
        const auto available = num_samples_ - start;
        if (length > available) length = available;
        return BufferView(channels_, num_channels_, length, sample_offset_ + start);
    }

    /// Get a span over all samples in one channel.
    /// @param index  Zero-based channel index.
    std::span<SampleType> channel(std::size_t index) {
        return {channels_[index] + sample_offset_, num_samples_};
    }

    /// @copydoc channel(std::size_t)
    std::span<const SampleType> channel(std::size_t index) const {
        return {channels_[index] + sample_offset_, num_samples_};
    }

    /// Raw pointer to one channel's sample data.
    SampleType* channel_ptr(std::size_t index) { return channels_[index] + sample_offset_; }
    /// @copydoc channel_ptr(std::size_t)
    const SampleType* channel_ptr(std::size_t index) const { return channels_[index] + sample_offset_; }

    std::size_t num_channels() const { return num_channels_; }
    std::size_t num_samples() const { return num_samples_; }

    /// True if the buffer has zero channels or zero samples.
    bool empty() const { return num_channels_ == 0 || num_samples_ == 0; }

    /// Zero-fill all channels.
    void clear() {
        for (std::size_t ch = 0; ch < num_channels_; ++ch) {
            std::fill_n(channels_[ch], num_samples_, SampleType{0});
        }
    }

private:
    BufferView(SampleType* const* channel_ptrs, std::size_t num_channels,
               std::size_t num_samples, std::size_t sample_offset)
        : channels_(channel_ptrs)
        , num_channels_(num_channels)
        , num_samples_(num_samples)
        , sample_offset_(sample_offset)
    {}

    SampleType* const* channels_ = nullptr;
    std::size_t num_channels_ = 0;
    std::size_t num_samples_ = 0;
    std::size_t sample_offset_ = 0;
};

/// Owning multi-channel audio buffer with contiguous storage.
///
/// Allocates and manages its own sample memory. Use for offline rendering,
/// test harnesses, and HeadlessHost — not in real-time audio callbacks where
/// allocation is forbidden. Call view() to get a non-owning BufferView.
///
/// @tparam SampleType  Sample format, typically @c float.
template<typename SampleType = float>
class Buffer {
public:
    Buffer() = default;

    /// Allocate a buffer with the given dimensions, zero-filled.
    /// @param num_channels  Number of audio channels.
    /// @param num_samples   Number of samples per channel.
    Buffer(std::size_t num_channels, std::size_t num_samples)
        : num_channels_(num_channels)
        , num_samples_(num_samples)
    {
        data_.resize(num_channels * num_samples, SampleType{0});
        ptrs_.resize(num_channels);
        for (std::size_t ch = 0; ch < num_channels; ++ch) {
            ptrs_[ch] = data_.data() + ch * num_samples;
        }
    }

    Buffer(const Buffer& other)
        : data_(other.data_)
        , num_channels_(other.num_channels_)
        , num_samples_(other.num_samples_) {
        refresh_channel_pointers();
    }

    Buffer& operator=(const Buffer& other) {
        if (this != &other) {
            data_ = other.data_;
            num_channels_ = other.num_channels_;
            num_samples_ = other.num_samples_;
            refresh_channel_pointers();
        }
        return *this;
    }

    Buffer(Buffer&& other)
        : data_(std::move(other.data_))
        , num_channels_(other.num_channels_)
        , num_samples_(other.num_samples_) {
        refresh_channel_pointers();
        other.num_channels_ = 0;
        other.num_samples_ = 0;
        other.ptrs_.clear();
    }

    Buffer& operator=(Buffer&& other) {
        if (this != &other) {
            data_ = std::move(other.data_);
            num_channels_ = other.num_channels_;
            num_samples_ = other.num_samples_;
            refresh_channel_pointers();
            other.num_channels_ = 0;
            other.num_samples_ = 0;
            other.ptrs_.clear();
        }
        return *this;
    }

    /// Resize the buffer, reallocating if necessary. New samples are zero-filled.
    void resize(std::size_t num_channels, std::size_t num_samples) {
        num_channels_ = num_channels;
        num_samples_ = num_samples;
        data_.resize(num_channels * num_samples, SampleType{0});
        refresh_channel_pointers();
    }

    /// Create a non-owning BufferView over this buffer's data.
    /// The view is invalidated if the Buffer is resized or destroyed.
    BufferView<SampleType> view() {
        return {ptrs_.data(), num_channels_, num_samples_};
    }

    /// @copydoc BufferView::channel(std::size_t)
    std::span<SampleType> channel(std::size_t index) {
        return {ptrs_[index], num_samples_};
    }

    /// @copydoc BufferView::channel(std::size_t) const
    std::span<const SampleType> channel(std::size_t index) const {
        return {ptrs_[index], num_samples_};
    }

    std::size_t num_channels() const { return num_channels_; }
    std::size_t num_samples() const { return num_samples_; }

    /// Zero-fill all channels.
    void clear() {
        std::fill(data_.begin(), data_.end(), SampleType{0});
    }

private:
    void refresh_channel_pointers() {
        ptrs_.resize(num_channels_);
        for (std::size_t ch = 0; ch < num_channels_; ++ch) {
            ptrs_[ch] = data_.data() + ch * num_samples_;
        }
    }

    std::vector<SampleType> data_;
    std::vector<SampleType*> ptrs_;
    std::size_t num_channels_ = 0;
    std::size_t num_samples_ = 0;
};

} // namespace pulp::audio

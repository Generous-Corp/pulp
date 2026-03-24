#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace pulp::audio {

// Non-owning multi-channel audio buffer view
// This is the primary type passed to audio callbacks.
// Does not allocate — it views memory owned elsewhere.
template<typename SampleType = float>
class BufferView {
public:
    BufferView() = default;

    BufferView(SampleType* const* channel_ptrs, std::size_t num_channels, std::size_t num_samples)
        : channels_(channel_ptrs)
        , num_channels_(num_channels)
        , num_samples_(num_samples)
    {}

    std::span<SampleType> channel(std::size_t index) {
        return {channels_[index], num_samples_};
    }

    std::span<const SampleType> channel(std::size_t index) const {
        return {channels_[index], num_samples_};
    }

    SampleType* channel_ptr(std::size_t index) { return channels_[index]; }
    const SampleType* channel_ptr(std::size_t index) const { return channels_[index]; }

    std::size_t num_channels() const { return num_channels_; }
    std::size_t num_samples() const { return num_samples_; }
    bool empty() const { return num_channels_ == 0 || num_samples_ == 0; }

    // Clear all channels to zero
    void clear() {
        for (std::size_t ch = 0; ch < num_channels_; ++ch) {
            std::fill_n(channels_[ch], num_samples_, SampleType{0});
        }
    }

private:
    SampleType* const* channels_ = nullptr;
    std::size_t num_channels_ = 0;
    std::size_t num_samples_ = 0;
};

// Owning multi-channel audio buffer
// Manages its own memory. Use for offline processing, test harnesses, etc.
template<typename SampleType = float>
class Buffer {
public:
    Buffer() = default;

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

    void resize(std::size_t num_channels, std::size_t num_samples) {
        num_channels_ = num_channels;
        num_samples_ = num_samples;
        data_.resize(num_channels * num_samples, SampleType{0});
        ptrs_.resize(num_channels);
        for (std::size_t ch = 0; ch < num_channels; ++ch) {
            ptrs_[ch] = data_.data() + ch * num_samples;
        }
    }

    BufferView<SampleType> view() {
        return {ptrs_.data(), num_channels_, num_samples_};
    }

    std::span<SampleType> channel(std::size_t index) {
        return {ptrs_[index], num_samples_};
    }

    std::span<const SampleType> channel(std::size_t index) const {
        return {ptrs_[index], num_samples_};
    }

    std::size_t num_channels() const { return num_channels_; }
    std::size_t num_samples() const { return num_samples_; }

    void clear() {
        std::fill(data_.begin(), data_.end(), SampleType{0});
    }

private:
    std::vector<SampleType> data_;
    std::vector<SampleType*> ptrs_;
    std::size_t num_channels_ = 0;
    std::size_t num_samples_ = 0;
};

} // namespace pulp::audio

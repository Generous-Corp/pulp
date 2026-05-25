#pragma once

// ProcessorDuplicator — apply a mono processor to multiple channels.
// Template that wraps a mono DSP processor and applies it independently
// to each channel of a multi-channel buffer.

#include <algorithm>
#include <cstddef>
#include <vector>

namespace pulp::signal {

/// Duplicates a mono processor across N channels.
/// The processor type P must support:
///   - default constructor
///   - set_sample_rate(float)
///   - process(float) -> float  (single-sample)
///   - reset()
template<typename P>
class ProcessorDuplicator {
public:
    ProcessorDuplicator() = default;

    /// Prepare for processing with the given number of channels
    void prepare(int num_channels, float sample_rate) {
        processors_.resize(static_cast<std::size_t>(std::max(0, num_channels)));
        for (auto& p : processors_) {
            p.set_sample_rate(sample_rate);
        }
    }

    /// Process a multi-channel buffer in-place
    /// channels: array of channel pointers
    /// num_channels: number of channels
    /// num_samples: samples per channel
    void process(float* const* channels, int num_channels, int num_samples) {
        if (channels == nullptr || num_channels <= 0 || num_samples <= 0)
            return;

        int count = std::min(num_channels, static_cast<int>(processors_.size()));
        for (int ch = 0; ch < count; ++ch) {
            if (channels[ch] == nullptr)
                continue;

            for (int i = 0; i < num_samples; ++i) {
                channels[ch][i] = processors_[ch].process(channels[ch][i]);
            }
        }
    }

    /// Process a single channel
    void process_channel(float* buffer, int channel, int num_samples) {
        if (buffer == nullptr || num_samples <= 0 ||
            channel < 0 || channel >= static_cast<int>(processors_.size())) return;

        for (int i = 0; i < num_samples; ++i) {
            buffer[i] = processors_[channel].process(buffer[i]);
        }
    }

    /// Access individual channel processor (e.g., to set parameters)
    P& operator[](int channel) { return processors_[static_cast<size_t>(channel)]; }
    const P& operator[](int channel) const { return processors_[static_cast<size_t>(channel)]; }

    /// Set parameters on all channel processors
    template<typename Func>
    void for_each(Func&& fn) {
        for (auto& p : processors_)
            fn(p);
    }

    /// Reset all channel processors
    void reset() {
        for (auto& p : processors_)
            p.reset();
    }

    int num_channels() const { return static_cast<int>(processors_.size()); }

private:
    std::vector<P> processors_;
};

}  // namespace pulp::signal

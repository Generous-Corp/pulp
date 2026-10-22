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
///
/// When P also provides an in-place block path `process(SampleType*, int)`,
/// each channel is handed to it whole instead of being looped through the
/// single-sample call, so a processor whose block path keeps its state in
/// registers gets that benefit here too. P's block path must be equivalent to
/// looping its single-sample path.
///
/// RT contract: `prepare()` resizes channel storage and is not audio-callback
/// safe. `process()`, `process_channel()`, `for_each()`, and `reset()` allocate
/// no memory after prepare when `P` follows the same contract.
template<typename P, typename SampleType = float>
class ProcessorDuplicatorT {
public:
    ProcessorDuplicatorT() = default;

    /// Prepare for processing with the given number of channels
    void prepare(int num_channels, SampleType sample_rate) {
        processors_.resize(static_cast<std::size_t>(std::max(0, num_channels)));
        for (auto& p : processors_) {
            p.set_sample_rate(sample_rate);
        }
    }

    /// Process a multi-channel buffer in-place
    /// channels: array of channel pointers
    /// num_channels: number of channels
    /// num_samples: samples per channel
    void process(SampleType* const* channels, int num_channels, int num_samples) {
        if (channels == nullptr || num_channels <= 0 || num_samples <= 0)
            return;

        int count = std::min(num_channels, static_cast<int>(processors_.size()));
        for (int ch = 0; ch < count; ++ch) {
            if (channels[ch] == nullptr)
                continue;

            process_buffer(processors_[ch], channels[ch], num_samples);
        }
    }

    /// Process a single channel
    void process_channel(SampleType* buffer, int channel, int num_samples) {
        if (buffer == nullptr || num_samples <= 0 ||
            channel < 0 || channel >= static_cast<int>(processors_.size())) return;

        process_buffer(processors_[static_cast<std::size_t>(channel)], buffer, num_samples);
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
    static void process_buffer(P& processor, SampleType* buffer, int num_samples) {
        if constexpr (requires { processor.process(buffer, num_samples); }) {
            processor.process(buffer, num_samples);
        } else {
            for (int i = 0; i < num_samples; ++i)
                buffer[i] = processor.process(buffer[i]);
        }
    }

    std::vector<P> processors_;
};

template<typename P>
using ProcessorDuplicator = ProcessorDuplicatorT<P, float>;

template<typename P>
using ProcessorDuplicator64 = ProcessorDuplicatorT<P, double>;

}  // namespace pulp::signal

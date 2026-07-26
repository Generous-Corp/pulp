#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

namespace pulp::signal {

// Delay line with linear interpolation for fractional delays.
// RT contract: prepare() allocates storage; push/read/process/reset allocate no
// memory after prepare().
template <typename SampleType = float>
class DelayLineT {
public:
    // Allocate buffer for max delay in samples
    void prepare(int max_delay_samples) {
        buffer_.assign(max_delay_samples + 1, SampleType{0.0f});
        write_pos_ = 0;
        valid_samples_ = 0;
    }

    // Push a sample into the delay line
    void push(SampleType sample) {
        if (buffer_.empty()) return;
        buffer_[write_pos_] = sample;
        write_pos_ = (write_pos_ + 1) % static_cast<int>(buffer_.size());
        valid_samples_ = std::min(valid_samples_ + 1, static_cast<int>(buffer_.size()));
    }

    // Read at a fractional delay (in samples) with linear interpolation
    SampleType read(SampleType delay_samples) const {
        int size = static_cast<int>(buffer_.size());
        if (size == 0 || !std::isfinite(static_cast<double>(delay_samples)) ||
            delay_samples < SampleType{0})
            return SampleType{0.0f};

        const int younger = static_cast<int>(std::floor(delay_samples));
        const SampleType frac = delay_samples - static_cast<SampleType>(younger);
        const SampleType a = read(younger);
        const SampleType b = read(younger + 1);
        return a * (SampleType{1.0f} - frac) + b * frac;
    }

    // Read at integer delay
    SampleType read(int delay_samples) const {
        int size = static_cast<int>(buffer_.size());
        if (size == 0 || delay_samples < 0 ||
            (valid_samples_ < size && delay_samples >= valid_samples_))
            return SampleType{0.0f};
        int idx = (write_pos_ - delay_samples - 1 + size * 2) % size;
        return buffer_[idx];
    }

    // Push a sample and read at a fixed delay
    SampleType process(SampleType input, SampleType delay_samples) {
        push(input);
        return read(delay_samples);
    }

    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), SampleType{0.0f});
        write_pos_ = 0;
        valid_samples_ = 0;
    }

    /// Logically clears the delay history in constant time. Reads remain zero
    /// until enough new samples have been pushed to cover the requested delay;
    /// the old allocation is overwritten naturally as processing resumes.
    void discard_history() noexcept {
        write_pos_ = 0;
        valid_samples_ = 0;
    }

    int max_delay() const { return static_cast<int>(buffer_.size()) - 1; }

private:
    std::vector<SampleType> buffer_;
    int write_pos_ = 0;
    int valid_samples_ = 0;
};

using DelayLine = DelayLineT<float>;
using DelayLine64 = DelayLineT<double>;

} // namespace pulp::signal

#pragma once

// The delay primitive every recursive stage in the tank is built from.
//
// signal::DelayLineT already exists and is the right block for ordinary delay
// work, but the tank needs two things it does not offer: a 4-point Hermite read
// (linear interpolation zippers audibly on the slow, continuous read-position
// sweeps this reverb runs on every one of its 16 lines) and an index step with
// no integer modulo, because the inner loop performs on the order of 300 reads
// per sample. So this is a power-of-two-masked ring with both read paths.
//
// RT contract: prepare() allocates; push/read/reset allocate nothing.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <pulp/signal/fdn/interp.hpp>

namespace pulp::signal::fdn {

template <typename SampleType = float>
class FracDelayT {
public:
    // Rounds up to a power of two with room for the Hermite window, so a read
    // at the maximum delay can still reach its two older neighbours.
    void prepare(int max_delay_samples) {
        const int needed = std::max(max_delay_samples, 1) + 4;
        std::size_t size = 8;
        while (size < static_cast<std::size_t>(needed)) size <<= 1;
        buffer_.assign(size, SampleType{0});
        mask_ = size - 1;
        write_ = 0;
        max_delay_ = static_cast<int>(size) - 4;
    }

    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), SampleType{0});
        write_ = 0;
    }

    int max_delay() const { return max_delay_; }

    void push(SampleType x) {
        buffer_[write_] = x;
        write_ = (write_ + 1) & mask_;
    }

    // Sample `delay` samples old (1 = the sample pushed most recently).
    SampleType read(int delay) const {
        const std::size_t idx = (write_ - static_cast<std::size_t>(delay)) & mask_;
        return buffer_[idx];
    }

    // Fractional read. `delay` is clamped into the window where all four
    // Hermite taps exist, so an out-of-range modulation excursion degrades to
    // the nearest legal read instead of walking off the allocation.
    SampleType read(double delay) const {
        const double d = std::clamp(delay, 2.0, static_cast<double>(max_delay_));
        const int i = static_cast<int>(d);
        const double f = d - static_cast<double>(i);
        // Older samples sit at larger delays, so the time-ordered Hermite taps
        // run from delay i+2 (oldest) down to i-1 (newest); the fractional
        // position runs the other way, hence 1 - f.
        return static_cast<SampleType>(hermite4(
            static_cast<double>(read(i + 2)), static_cast<double>(read(i + 1)),
            static_cast<double>(read(i)), static_cast<double>(read(i - 1)), 1.0 - f));
    }

private:
    std::vector<SampleType> buffer_;
    std::size_t mask_ = 0;
    std::size_t write_ = 0;
    int max_delay_ = 0;
};

using FracDelay = FracDelayT<float>;

}  // namespace pulp::signal::fdn

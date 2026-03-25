#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

namespace pulp::signal {

// Delay line with linear interpolation for fractional delays
// Real-time safe after prepare() (allocates buffer)
class DelayLine {
public:
    // Allocate buffer for max delay in samples
    void prepare(int max_delay_samples) {
        buffer_.assign(max_delay_samples + 1, 0.0f);
        write_pos_ = 0;
    }

    // Push a sample into the delay line
    void push(float sample) {
        buffer_[write_pos_] = sample;
        write_pos_ = (write_pos_ + 1) % static_cast<int>(buffer_.size());
    }

    // Read at a fractional delay (in samples) with linear interpolation
    float read(float delay_samples) const {
        int size = static_cast<int>(buffer_.size());
        if (size == 0) return 0.0f;

        float read_pos = static_cast<float>(write_pos_) - delay_samples - 1.0f;
        while (read_pos < 0) read_pos += size;

        int idx0 = static_cast<int>(read_pos) % size;
        int idx1 = (idx0 + 1) % size;
        float frac = read_pos - std::floor(read_pos);

        return buffer_[idx0] * (1.0f - frac) + buffer_[idx1] * frac;
    }

    // Read at integer delay
    float read(int delay_samples) const {
        int size = static_cast<int>(buffer_.size());
        if (size == 0) return 0.0f;
        int idx = (write_pos_ - delay_samples - 1 + size * 2) % size;
        return buffer_[idx];
    }

    // Push a sample and read at a fixed delay
    float process(float input, float delay_samples) {
        push(input);
        return read(delay_samples);
    }

    void reset() {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_pos_ = 0;
    }

    int max_delay() const { return static_cast<int>(buffer_.size()) - 1; }

private:
    std::vector<float> buffer_;
    int write_pos_ = 0;
};

} // namespace pulp::signal

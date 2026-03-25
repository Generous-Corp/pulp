#pragma once

#include <string>
#include <atomic>
#include <functional>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace pulp::state {

// Unique parameter identifier — stable across plugin versions
using ParamID = uint32_t;

// Parameter range with normalization
struct ParamRange {
    float min = 0.0f;
    float max = 1.0f;
    float default_value = 0.0f;
    float step = 0.0f; // 0 = continuous

    float normalize(float value) const {
        if (max == min) return 0.0f;
        return std::clamp((value - min) / (max - min), 0.0f, 1.0f);
    }

    float denormalize(float normalized) const {
        auto value = min + normalized * (max - min);
        if (step > 0.0f) {
            value = min + std::round((value - min) / step) * step;
        }
        return std::clamp(value, min, max);
    }
};

// Parameter definition — immutable metadata
struct ParamInfo {
    ParamID id = 0;
    std::string name;
    std::string unit;       // "dB", "Hz", "%", etc.
    ParamRange range;
    int group_id = 0;       // 0 = ungrouped

    // String conversion for display
    std::function<std::string(float)> to_string;
    std::function<float(const std::string&)> from_string;
};

// Thread-safe parameter value
// Audio thread reads via atomic. UI thread writes via atomic.
// No locks. No allocation.
class ParamValue {
public:
    ParamValue() = default;
    explicit ParamValue(float initial) : value_(initial) {}

    // Atomics aren't movable, so provide explicit move support
    ParamValue(ParamValue&& other) noexcept : value_(other.value_.load()) {}
    ParamValue& operator=(ParamValue&& other) noexcept {
        value_.store(other.value_.load());
        return *this;
    }
    ParamValue(const ParamValue& other) : value_(other.value_.load()) {}
    ParamValue& operator=(const ParamValue& other) {
        value_.store(other.value_.load());
        return *this;
    }

    float get() const { return value_.load(std::memory_order_relaxed); }
    void set(float v) { value_.store(v, std::memory_order_relaxed); }

    // Modulated value = base + mod_offset (for CLAP per-voice modulation)
    float get_modulated() const {
        return value_.load(std::memory_order_relaxed)
             + mod_offset_.load(std::memory_order_relaxed);
    }
    void set_mod_offset(float offset) { mod_offset_.store(offset, std::memory_order_relaxed); }
    void add_mod_offset(float delta) {
        auto current = mod_offset_.load(std::memory_order_relaxed);
        mod_offset_.store(current + delta, std::memory_order_relaxed);
    }
    void reset_mod() { mod_offset_.store(0.0f, std::memory_order_relaxed); }

    float get_normalized(const ParamRange& range) const {
        return range.normalize(get());
    }

    void set_normalized(float n, const ParamRange& range) {
        set(range.denormalize(n));
    }

private:
    std::atomic<float> value_{0.0f};
    std::atomic<float> mod_offset_{0.0f};
};

// Change listener — notified when a parameter value changes
using ParamChangeCallback = std::function<void(ParamID id, float new_value)>;

} // namespace pulp::state

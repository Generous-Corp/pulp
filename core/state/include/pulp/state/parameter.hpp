#pragma once

#include <string>
#include <atomic>
#include <functional>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace pulp::state {

/// Unique parameter identifier, stable across plugin versions.
/// Use a hash or manual assignment to keep IDs consistent between releases.
using ParamID = uint32_t;

/// Defines the numeric range of a parameter, including normalization.
///
/// Normalization maps the [min, max] range to [0, 1] for host automation.
/// If @c step is non-zero, denormalized values snap to the nearest step.
struct ParamRange {
    float min = 0.0f;
    float max = 1.0f;
    float default_value = 0.0f;
    float step = 0.0f; ///< Quantization step. 0 = continuous.

    /// Map a real value to the normalized [0, 1] range.
    /// @param value  Raw parameter value in [min, max].
    /// @return Normalized value clamped to [0, 1].
    float normalize(float value) const {
        if (max == min) return 0.0f;
        return std::clamp((value - min) / (max - min), 0.0f, 1.0f);
    }

    /// Map a normalized [0, 1] value back to the real range.
    /// Applies step quantization if step > 0.
    /// @param normalized  Value in [0, 1].
    /// @return Real value clamped to [min, max], optionally quantized.
    float denormalize(float normalized) const {
        auto value = min + normalized * (max - min);
        if (step > 0.0f) {
            value = min + std::round((value - min) / step) * step;
        }
        return std::clamp(value, min, max);
    }
};

/// Immutable parameter metadata, registered once at plugin initialization.
///
/// Each parameter has a unique @c id, a human-readable @c name, an optional
/// @c unit string for display, a @c range for normalization, and optional
/// string conversion functions for host display.
struct ParamInfo {
    ParamID id = 0;
    std::string name;
    std::string unit;       ///< Display unit: "dB", "Hz", "%", etc.
    ParamRange range;
    int group_id = 0;       ///< Group for hierarchical organization. 0 = ungrouped.

    /// Convert a raw value to a display string (e.g., "440.0 Hz").
    std::function<std::string(float)> to_string;
    /// Parse a display string back to a raw value.
    std::function<float(const std::string&)> from_string;
};

/// Thread-safe atomic parameter value for lock-free audio/UI communication.
///
/// The audio thread reads and the UI thread writes via relaxed atomics.
/// No locks, no allocation. Each parameter is independent, so relaxed
/// ordering is correct — there is no dependency between reading param A
/// and param B. For coherent multi-field reads (e.g., transport state),
/// use SeqLock<T> instead.
///
/// @note Relaxed atomics are safe on both x86 (TSO gives acquire/release
/// for free) and ARM (each load/store is individually atomic; reordering
/// between independent parameters is harmless).
class ParamValue {
public:
    ParamValue() = default;

    /// @param initial  Starting value for this parameter.
    explicit ParamValue(float initial) : value_(initial) {}

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

    /// Read the current base value (lock-free, relaxed ordering).
    float get() const { return value_.load(std::memory_order_relaxed); }

    /// Write a new base value (lock-free, relaxed ordering).
    void set(float v) { value_.store(v, std::memory_order_relaxed); }

    /// Read the modulated value: base + mod_offset.
    /// Used by the audio thread to get the final value after CLAP per-voice modulation.
    float get_modulated() const {
        return value_.load(std::memory_order_relaxed)
             + mod_offset_.load(std::memory_order_relaxed);
    }

    /// Set the absolute modulation offset (replaces any existing offset).
    void set_mod_offset(float offset) { mod_offset_.store(offset, std::memory_order_relaxed); }

    /// Add a delta to the current modulation offset (for stacking modulators).
    /// @note Not atomic with respect to concurrent add_mod_offset calls;
    ///       assumes a single writer (the audio thread).
    void add_mod_offset(float delta) {
        auto current = mod_offset_.load(std::memory_order_relaxed);
        mod_offset_.store(current + delta, std::memory_order_relaxed);
    }

    /// Clear the modulation offset to zero.
    void reset_mod() { mod_offset_.store(0.0f, std::memory_order_relaxed); }

    /// Read the current value mapped to [0, 1] via the given range.
    float get_normalized(const ParamRange& range) const {
        return range.normalize(get());
    }

    /// Write a normalized [0, 1] value, denormalized via the given range.
    void set_normalized(float n, const ParamRange& range) {
        set(range.denormalize(n));
    }

private:
    std::atomic<float> value_{0.0f};
    std::atomic<float> mod_offset_{0.0f};
};

/// Callback signature for parameter change notifications.
/// @param id         The parameter that changed.
/// @param new_value  The new raw value.
using ParamChangeCallback = std::function<void(ParamID id, float new_value)>;

} // namespace pulp::state

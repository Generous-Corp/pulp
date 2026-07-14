#pragma once

#include <string>
#include <atomic>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <cctype>

namespace pulp::state {

/// Unique parameter identifier, stable across plugin versions.
/// Use a hash or manual assignment to keep IDs consistent between releases.
using ParamID = uint32_t;

/// Parameter update-rate classification.
///
/// ControlRate parameters are read as one coherent value per processing
/// block. AudioRate parameters may accept dense per-sample modulation from
/// host graph edges.
enum class ParamRate {
    ControlRate,
    AudioRate,
};

/// Author-declared parameter semantics.
///
/// This is deliberately independent from ParamRange::step. A continuous
/// parameter may still snap to a grid, while integer, toggle, and enum
/// parameters must be advertised to hosts as discrete even if their range was
/// assembled by compatibility code that omitted a step.
enum class ParamKind {
    Continuous,
    Integer,
    Toggle,
    Enum,
};

/// Declared semantic role of a parameter.
///
/// A designation lets the Processor author state *what a parameter means*
/// rather than leaving format adapters to guess from its name and range.
/// Adapters consult the declared designation first and fall back to a
/// name/range heuristic only for parameters that declare `None`, so existing
/// plugins keep working unchanged.
///
/// - `None`   — an ordinary parameter (the default). Adapters apply their
///              legacy name/range heuristic to decide whether it is a bypass.
/// - `Bypass` — a host-visible bypass control. Interpreted as a toggle with
///              the threshold convention **off < 0.5, on >= 0.5**, regardless
///              of the declared range: adapters short-circuit to pass-through
///              audio when the current value is >= 0.5. Because of this
///              threshold, format adapters present a declared `Bypass`
///              parameter to the host as a two-state/boolean control even if
///              its `ParamRange` is continuous. Declare this independently of
///              the parameter's name.
/// - `Reset`  — a host-visible momentary "reset / panic" control. It behaves
///              as a trigger parameter (see `ParamInfo::is_trigger`): the host
///              sets it, the Processor observes it for one block, and the
///              value auto-resets to its default after the block.
enum class ParamDesignation {
    None,
    Bypass,
    Reset,
};

/// Defines the numeric range of a parameter, including normalization.
///
/// Normalization maps the [min, max] range to [0, 1] for host automation.
/// If @c step is non-zero, denormalized values snap to the nearest step.
///
/// ## Shaped (non-linear) ranges
///
/// `skew` and `symmetric_skew` give the range a non-linear curve between the
/// [0, 1] host-automation domain and the [min, max] parameter domain:
///
/// - `skew == 1` (default) is **linear** — the conversion is bit-identical to
///   a plain affine map, so existing linear parameters are unchanged.
/// - `skew < 1` weights the curve toward `min` (small real values cover more
///   of the [0, 1] domain — e.g. a frequency knob where the low end gets more
///   resolution). `skew > 1` weights toward `max`.
/// - `symmetric_skew == true` mirrors the curve around the centre, so the
///   midpoint of the normalized domain lands at the midpoint of [min, max] and
///   both halves fan out symmetrically. Useful for bipolar parameters (pan,
///   detune, balance).
///
/// The shaping convention matches `SkewedRange<T>` below, which shares
/// the same math. Use `ParamRange::with_centre()` to derive the skew that
/// places a chosen real value at the normalized midpoint (0.5).
struct ParamRange {
    float min = 0.0f;
    float max = 1.0f;
    float default_value = 0.0f;
    float step = 0.0f;           ///< Quantization step. 0 = continuous.
    float skew = 1.0f;           ///< 1 = linear, <1 weights min, >1 weights max.
    bool symmetric_skew = false; ///< Mirror the curve around the centre (bipolar).

    /// Build a linear range covering [lo, hi] with the given default and step.
    /// Provided so call sites can be explicit about the linear case alongside
    /// the shaped factories below.
    static ParamRange linear(float lo, float hi, float def = 0.0f,
                             float step_value = 0.0f) {
        return ParamRange{lo, hi, def, step_value, 1.0f, false};
    }

    /// Build a shaped range whose normalized midpoint (0.5) maps to @p centre.
    /// Equivalent to choosing the skew that satisfies denormalize(0.5) == centre.
    /// Falls back to a linear range when @p centre is not strictly inside the
    /// open interval (min, max).
    static ParamRange with_centre(float lo, float hi, float centre,
                                  float def = 0.0f, float step_value = 0.0f) {
        ParamRange r{lo, hi, def, step_value, 1.0f, false};
        if (hi > lo && centre > lo && centre < hi) {
            const float proportion = (centre - lo) / (hi - lo);
            r.skew = std::log(0.5f) / std::log(proportion);
        }
        return r;
    }

    /// True when this range is a plain affine (linear) map — the common case.
    /// Used to take a bit-identical fast path that bypasses the skew math.
    bool is_linear() const { return skew == 1.0f && !symmetric_skew; }

    /// Map a real value to the normalized [0, 1] range.
    /// @param value  Raw parameter value in [min, max].
    /// @return Normalized value clamped to [0, 1].
    float normalize(float value) const {
        if (max == min) return 0.0f;
        // Bit-identical linear fast path — preserves legacy behavior exactly.
        if (is_linear()) {
            return std::clamp((value - min) / (max - min), 0.0f, 1.0f);
        }
        float proportion = std::clamp((value - min) / (max - min), 0.0f, 1.0f);
        if (symmetric_skew) {
            const float centred = (proportion * 2.0f) - 1.0f; // [-1, 1]
            const float magnitude = std::pow(std::abs(centred), skew);
            const float signed_mag = centred < 0.0f ? -magnitude : magnitude;
            return (signed_mag + 1.0f) * 0.5f;
        }
        return std::pow(proportion, skew);
    }

    /// Map a normalized [0, 1] value back to the real range.
    /// Applies step quantization if step > 0.
    /// @param normalized  Value in [0, 1].
    /// @return Real value clamped to [min, max], optionally quantized.
    float denormalize(float normalized) const {
        // Bit-identical linear fast path — preserves legacy behavior exactly.
        if (is_linear()) {
            auto value = min + normalized * (max - min);
            if (step > 0.0f) {
                value = min + std::round((value - min) / step) * step;
            }
            return std::clamp(value, min, max);
        }
        normalized = std::clamp(normalized, 0.0f, 1.0f);
        float proportion;
        if (symmetric_skew) {
            const float centred = (normalized * 2.0f) - 1.0f; // [-1, 1]
            const float magnitude = std::pow(std::abs(centred), 1.0f / skew);
            const float signed_mag = centred < 0.0f ? -magnitude : magnitude;
            proportion = (signed_mag + 1.0f) * 0.5f;
        } else {
            proportion = std::pow(normalized, 1.0f / skew);
        }
        float value = min + proportion * (max - min);
        if (step > 0.0f) {
            value = min + std::round((value - min) / step) * step;
        }
        return std::clamp(value, min, max);
    }
};

/// Skew-aware normalized range with optional symmetric centre.
///
/// `SkewedRange<T>` extends `ParamRange` with a non-linear mapping
/// between the [0, 1] host-automation domain and the [min, max] parameter
/// domain. Two shape controls:
///
/// - `skew`: <1 weights toward `min`, >1 weights toward `max`, 1 = linear.
///   Conventionally, a skew of `log(0.5) / log((mid - min) / (max - min))`
///   places `mid` at the centre (0.5) of the normalized range.
/// - `symmetric_skew`: when true, the curve is mirrored around the centre,
///   so values near 0.5 in normalized space land near the midpoint and
///   values near the edges fan out symmetrically. Useful for bipolar
///   parameters like pan, balance, detune.
///
/// Step quantization is honored on denormalize (matches `ParamRange::step`).
///
/// @code
/// // Frequency knob: 20 Hz at min, 20 kHz at max, 1 kHz at centre.
/// pulp::state::SkewedRange<float> freq{20.0f, 20000.0f, 1000.0f};
/// float normalized = freq.normalize(1000.0f); // ~= 0.5
/// float hz         = freq.denormalize(0.5f);  // ~= 1000.0f
/// @endcode
template<typename T = float>
struct SkewedRange {
    T min = T(0);
    T max = T(1);
    T interval = T(0);          ///< Step size. 0 = continuous.
    T skew = T(1);              ///< 1 = linear, <1 weights min, >1 weights max.
    bool symmetric_skew = false;

    constexpr SkewedRange() = default;
    constexpr SkewedRange(T lo, T hi, T step = T(0), T skew_value = T(1),
                                bool symmetric = false)
        : min(lo), max(hi), interval(step),
          skew(skew_value), symmetric_skew(symmetric) {}

    /// Build a SkewedRange whose normalised midpoint maps to the
    /// supplied real-valued centre. Equivalent to choosing the skew that
    /// satisfies denormalize(0.5) == centre.
    static SkewedRange with_centre(T lo, T hi, T centre, T step = T(0)) {
        SkewedRange r{lo, hi, step};
        if (hi > lo && centre > lo && centre < hi) {
            const T proportion = (centre - lo) / (hi - lo);
            // log(0.5) / log(proportion) gives the skew that lands `centre` at 0.5.
            r.skew = std::log(T(0.5)) / std::log(proportion);
        }
        return r;
    }

    /// Map a real value in [min, max] to a normalized value in [0, 1].
    ///
    /// Skew convention: with `skew < 1` the curve weights toward `min`
    /// (small real values cover more of the [0, 1] domain); with
    /// `skew > 1` the curve weights toward `max`. `skew == 1` is linear.
    T normalize(T value) const {
        if (max == min) return T(0);
        T proportion = (value - min) / (max - min);
        proportion = std::clamp(proportion, T(0), T(1));
        if (symmetric_skew) {
            const T centred = (proportion * T(2)) - T(1); // [-1, 1]
            const T magnitude = std::pow(std::abs(centred), skew);
            const T signed_mag = centred < T(0) ? -magnitude : magnitude;
            return (signed_mag + T(1)) * T(0.5);
        }
        return std::pow(proportion, skew);
    }

    /// Map a normalized [0, 1] value back to the real range. Applies
    /// `interval` quantization when non-zero.
    T denormalize(T normalized) const {
        normalized = std::clamp(normalized, T(0), T(1));
        T proportion;
        if (symmetric_skew) {
            const T centred = (normalized * T(2)) - T(1); // [-1, 1]
            const T magnitude = std::pow(std::abs(centred), T(1) / skew);
            const T signed_mag = centred < T(0) ? -magnitude : magnitude;
            proportion = (signed_mag + T(1)) * T(0.5);
        } else {
            proportion = std::pow(normalized, T(1) / skew);
        }
        T value = min + proportion * (max - min);
        if (interval > T(0)) {
            value = min + std::round((value - min) / interval) * interval;
        }
        return std::clamp(value, min, max);
    }

    /// Round a real value down to the nearest legal step.
    T snap(T value) const {
        if (interval <= T(0)) return std::clamp(value, min, max);
        T snapped = min + std::round((value - min) / interval) * interval;
        return std::clamp(snapped, min, max);
    }
};

/// Deprecated spelling of `SkewedRange<T>`. Kept so existing code keeps
/// compiling; the type is identical. Prefer `SkewedRange<T>` — it names the
/// thing the type actually is (a min/max range plus a skew curve).
template<typename T = float>
using NormalisableRange [[deprecated("renamed to pulp::state::SkewedRange")]] = SkewedRange<T>;

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

    ParamRate rate = ParamRate::ControlRate;
    float smoothing_ramp_seconds = 0.0f; ///< 0 = off / immediate control-rate updates.

    /// Declared semantic role. `None` (the default) preserves legacy behavior:
    /// adapters fall back to their name/range bypass heuristic. Declaring
    /// `Bypass` or `Reset` makes the role explicit and name-independent.
    ParamDesignation designation = ParamDesignation::None;

    /// Momentary "trigger" parameter. A trigger is a one-shot "do this now"
    /// control (panic, reset, tap): the host or UI raises it, the Processor
    /// observes it for exactly one process block, and the state plumbing
    /// auto-resets it to its range default after the block
    /// (see `StateStore::reset_triggers_rt`). A `Reset` designation implies a
    /// trigger; an author may also set this directly for a custom one-shot.
    ///
    /// One-shot latch contract: a trigger value is **block-local**. Whatever
    /// value is present during a process block — raised by the host/UI, or even
    /// written by the Processor itself mid-block — is reset to the range default
    /// at the END of that block. A trigger therefore never persists across
    /// blocks; observe it within the block it fires. Format adapters reset
    /// triggers on EVERY audio-callback exit, including bypass / pass-through
    /// early-returns, so a panic/reset raised while bypassed still clears that
    /// block rather than firing late on the next non-bypassed block.
    bool is_trigger = false;

    /// Semantic value kind. Existing declarations remain continuous until
    /// they opt in; adapters must not infer this from the parameter name.
    ParamKind kind = ParamKind::Continuous;

    /// Ordered display/parse labels for Toggle and Enum parameters. Index zero
    /// corresponds to range.min, index one to range.min + range.step (or +1
    /// when the legacy range omitted a step), and so on.
    std::vector<std::string> value_labels;

    /// True when this parameter should auto-reset to its default after each
    /// process block — i.e. it is an explicit trigger or carries the `Reset`
    /// designation (which is defined to behave as a trigger).
    bool auto_resets() const {
        return is_trigger || designation == ParamDesignation::Reset;
    }
};

inline bool is_discrete_param(const ParamInfo& info) {
    // Semantic kind is author-declared metadata. A continuous parameter may
    // still use range.step to quantize its plain value without being exposed
    // to hosts as an integer/indexed control.
    return info.kind != ParamKind::Continuous;
}

inline bool is_boolean_param(const ParamInfo& info) {
    return info.kind == ParamKind::Toggle;
}

inline uint32_t param_value_count(const ParamInfo& info) {
    if (!info.value_labels.empty())
        return static_cast<uint32_t>(info.value_labels.size());
    if (!is_discrete_param(info)) return 0;
    const float step = info.range.step > 0.0f ? info.range.step : 1.0f;
    const float span = std::max(0.0f, info.range.max - info.range.min);
    return static_cast<uint32_t>(std::llround(span / step)) + 1u;
}

inline float constrain_param_value(const ParamInfo& info, float value) {
    if (!std::isfinite(value)) return info.range.default_value;
    value = std::clamp(value, info.range.min, info.range.max);
    const float step = info.range.step > 0.0f
        ? info.range.step
        : (is_discrete_param(info) ? 1.0f : 0.0f);
    if (step > 0.0f) {
        value = info.range.min
            + std::round((value - info.range.min) / step) * step;
    }
    return std::clamp(value, info.range.min, info.range.max);
}

inline std::optional<float> param_value_for_label(const ParamInfo& info,
                                                   std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    const float step = info.range.step > 0.0f ? info.range.step : 1.0f;
    for (std::size_t i = 0; i < info.value_labels.size(); ++i) {
        if (text == info.value_labels[i]) {
            return constrain_param_value(
                info, info.range.min + static_cast<float>(i) * step);
        }
    }
    return std::nullopt;
}

/// Decide whether @p info is a bypass control.
///
/// Designation-first, heuristic-fallback: a declared `Bypass` designation
/// wins regardless of name or range. A parameter that declares `None` falls
/// back to the legacy name/range heuristic (a boolean parameter named
/// "Bypass": step >= 1, range exactly [0, 1]) so existing plugins are
/// unchanged. A non-`None`, non-`Bypass` designation is never a bypass.
///
/// This is the single source of truth shared by every format adapter, so the
/// bypass contract cannot drift between VST3 / AU / CLAP.
inline bool is_bypass_param(const ParamInfo& info) {
    if (info.designation == ParamDesignation::Bypass) return true;
    if (info.designation != ParamDesignation::None) return false;
    return info.name == "Bypass" && info.range.step >= 1.0f &&
           info.range.min == 0.0f && info.range.max == 1.0f;
}

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

    // value_ / mod_offset_ are read and written on the audio thread through
    // relaxed std::atomic<float> (see get()/set()/get_modulated()). If
    // std::atomic<float> were not lock-free on a target, those accessors would
    // fall back to a hidden mutex and violate the audio-thread no-lock contract.
    // Fail the build instead. See docs/guides/dsp-threading.md "The three rules".
    static_assert(std::atomic<float>::is_always_lock_free,
                  "Pulp requires a lock-free std::atomic<float> for real-time-safe "
                  "parameter access on the audio thread");
};

/// Callback signature for parameter change notifications.
/// @param id         The parameter that changed.
/// @param new_value  The new raw value.
using ParamChangeCallback = std::function<void(ParamID id, float new_value)>;

} // namespace pulp::state

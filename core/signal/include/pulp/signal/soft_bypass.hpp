#pragma once

/// @file soft_bypass.hpp
/// Click-free bypass wrapper for a DSP processor.
///
/// Toggling a processor in/out of the signal path with a hard switch clicks,
/// because the output jumps between the processed and dry signal. SoftBypass
/// wraps any processor that exposes `float process(float)` and crossfades
/// between its wet output and the dry input over a configurable fade time, so
/// engaging or bypassing is smooth.
///
/// It composes with the existing ProcessorChain — `SoftBypass<ProcessorChain<...>>`
/// gives a whole chain a click-free bypass — making it a small container/wrapper
/// building block rather than a one-off.
///
/// Note: while fully bypassed the wrapped processor is skipped entirely, so its
/// internal state freezes. For memoryless / short-state processors the dry/wet
/// crossfade on re-engage is click-free. For long-tail / feedback processors
/// (delays, reverbs) the frozen state resumes on re-engage, which the mix
/// crossfade cannot fully smooth — keep such a processor running through the
/// bypass (e.g. wrap a dry/wet level rather than SoftBypass) if tail continuity
/// matters.
///
/// RT contract: process()/set_bypassed()/set_fade_samples() are allocation-free
/// and audio-thread safe when the wrapped processor is. set_fade_samples() and
/// the bypass target are plain stores; the fade is applied per sample.

#include <algorithm>
#include <cstddef>
#include <utility>

namespace pulp::signal {

template <typename Processor, typename SampleType = float>
class SoftBypassT {
public:
    SoftBypassT() = default;
    explicit SoftBypassT(Processor processor) : processor_(std::move(processor)) {}

    /// Access the wrapped processor. SoftBypass forwards reset() but NOT
    /// prepare()/sample-rate setup — a wrapped processor that needs preparation
    /// must be prepared through this accessor before use.
    Processor& processor() noexcept { return processor_; }
    const Processor& processor() const noexcept { return processor_; }

    /// Crossfade length in samples for a bypass toggle (clamped to >= 1).
    void set_fade_samples(int samples) noexcept {
        fade_step_ = SampleType{1.0f} /
                     static_cast<SampleType>(samples < 1 ? 1 : samples);
    }

    /// Request bypassed (true) or active (false). The change crossfades in.
    void set_bypassed(bool bypassed) noexcept {
        target_wet_ = bypassed ? SampleType{0.0f} : SampleType{1.0f};
    }
    /// True once fully (or heading) bypassed.
    bool bypassed() const noexcept { return target_wet_ <= SampleType{0.0f}; }
    /// True while a crossfade is still in progress.
    bool fading() const noexcept { return current_wet_ != target_wet_; }
    /// Current wet mix in [0, 1] (1 = fully active, 0 = fully bypassed).
    SampleType wet_mix() const noexcept { return current_wet_; }

    /// Process one sample with the current (possibly fading) bypass mix.
    SampleType process(SampleType input) noexcept {
        advance_fade();
        // Fully bypassed and settled: skip the wrapped processor entirely. The
        // fade-in on re-engage ramps the wet contribution up from zero, so this
        // stays click-free despite the processor's state being frozen meanwhile.
        if (current_wet_ <= SampleType{0.0f}) return input;
        const SampleType wet = processor_.process(input);
        if (current_wet_ >= SampleType{1.0f}) return wet;
        return input * (SampleType{1.0f} - current_wet_) + wet * current_wet_;
    }

    /// Process a buffer in place.
    void process(SampleType* data, int num_samples) noexcept {
        for (int i = 0; i < num_samples; ++i) data[i] = process(data[i]);
    }

    /// Reset the wrapped processor (if it supports it) and settle the fade.
    void reset() noexcept {
        if constexpr (requires { processor_.reset(); }) processor_.reset();
        current_wet_ = target_wet_;
    }

private:
    void advance_fade() noexcept {
        if (current_wet_ < target_wet_) {
            current_wet_ = std::min(target_wet_, current_wet_ + fade_step_);
        } else if (current_wet_ > target_wet_) {
            current_wet_ = std::max(target_wet_, current_wet_ - fade_step_);
        }
    }

    Processor processor_{};
    SampleType current_wet_ = SampleType{1.0f};  // start active
    SampleType target_wet_ = SampleType{1.0f};
    SampleType fade_step_ = SampleType{1.0f} / SampleType{256.0f};  // ~256-sample default fade
};

template <typename Processor>
using SoftBypass = SoftBypassT<Processor, float>;

template <typename Processor>
using SoftBypass64 = SoftBypassT<Processor, double>;

}  // namespace pulp::signal

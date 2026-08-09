#pragma once

/// @file transient_designer.hpp
/// Zero-latency transient shaping from independent fast and slow envelopes.

#include <pulp/signal/dynamics_contract.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// Separates attack and sustain by comparing exact-timing peak envelopes.
///
/// A positive fast-minus-slow contrast identifies an onset. A negative
/// contrast identifies the body after a level falls. `attack_db` and
/// `sustain_db` scale those two regions independently; positive values enhance
/// and negative values attenuate. Both controls at zero are exactly transparent.
///
/// This is a zero-lookahead, zero-latency primitive. It owns fixed scalar state
/// and `process()`, block processing, `reset()`, and accessors allocate no
/// memory. Call `prepare()` and the setters from control code, not concurrently
/// with the audio callback.
template <typename SampleType = float> class TransientDesignerT {
  public:
    static constexpr SampleType kMinShapeDb = SampleType{-24};
    static constexpr SampleType kMaxShapeDb = SampleType{24};

    void prepare(SampleType sample_rate) {
        if (!std::isfinite(static_cast<double>(sample_rate)) || !(sample_rate > SampleType{0}))
            sample_rate = SampleType{44100};
        fast_.prepare(sample_rate);
        slow_.prepare(sample_rate);
        apply_detector_times_();
        reset();
    }

    void set_attack_db(SampleType db) noexcept {
        if (std::isfinite(static_cast<double>(db)))
            attack_db_ = std::clamp(db, kMinShapeDb, kMaxShapeDb);
    }

    void set_sustain_db(SampleType db) noexcept {
        if (std::isfinite(static_cast<double>(db)))
            sustain_db_ = std::clamp(db, kMinShapeDb, kMaxShapeDb);
    }

    void set_fast_attack_ms(SampleType ms) {
        set_time_(ms, fast_attack_ms_, true, true);
    }
    void set_fast_release_ms(SampleType ms) {
        set_time_(ms, fast_release_ms_, true, false);
    }
    void set_slow_attack_ms(SampleType ms) {
        set_time_(ms, slow_attack_ms_, false, true);
    }
    void set_slow_release_ms(SampleType ms) {
        set_time_(ms, slow_release_ms_, false, false);
    }

    SampleType process(SampleType input) noexcept {
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{0};
        }

        const SampleType fast = fast_.process(input);
        const SampleType slow = slow_.process(input);
        const SampleType scale = std::max({fast, slow, SampleType{1.0e-12}});
        contrast_ = std::clamp((fast - slow) / scale, SampleType{-1}, SampleType{1});

        const SampleType attack_amount = std::max(contrast_, SampleType{0});
        const SampleType sustain_amount = std::max(-contrast_, SampleType{0});
        gain_db_ = attack_db_ * attack_amount + sustain_db_ * sustain_amount;

        // Preserve an exact transparent path while still advancing detector
        // state, so automation can enter from neutral without a stale onset.
        if (attack_db_ == SampleType{0} && sustain_db_ == SampleType{0})
            return input;
        const SampleType output = input * std::pow(SampleType{10}, gain_db_ / SampleType{20});
        if (!std::isfinite(static_cast<double>(output))) {
            reset();
            return SampleType{0};
        }
        return output;
    }

    void process(const SampleType* input, SampleType* output, int num_samples) noexcept {
        if (input == nullptr || output == nullptr || num_samples <= 0)
            return;
        for (int i = 0; i < num_samples; ++i)
            output[i] = process(input[i]);
    }

    void reset() noexcept {
        fast_.reset();
        slow_.reset();
        contrast_ = SampleType{0};
        gain_db_ = SampleType{0};
    }

    SampleType attack_db() const noexcept {
        return attack_db_;
    }
    SampleType sustain_db() const noexcept {
        return sustain_db_;
    }
    SampleType fast_attack_ms() const noexcept {
        return fast_attack_ms_;
    }
    SampleType fast_release_ms() const noexcept {
        return fast_release_ms_;
    }
    SampleType slow_attack_ms() const noexcept {
        return slow_attack_ms_;
    }
    SampleType slow_release_ms() const noexcept {
        return slow_release_ms_;
    }
    SampleType contrast() const noexcept {
        return contrast_;
    }
    SampleType gain_db() const noexcept {
        return gain_db_;
    }
    static constexpr int latency_samples() noexcept {
        return 0;
    }

  private:
    void set_time_(SampleType ms, SampleType& stored, bool fast, bool attack) {
        if (!std::isfinite(static_cast<double>(ms)))
            return;
        stored = std::max(ms, SampleType{0.01});
        auto& follower = fast ? fast_ : slow_;
        if (attack)
            follower.set_attack_ms(stored);
        else
            follower.set_release_ms(stored);
    }

    void apply_detector_times_() {
        fast_.set_attack_ms(fast_attack_ms_);
        fast_.set_release_ms(fast_release_ms_);
        slow_.set_attack_ms(slow_attack_ms_);
        slow_.set_release_ms(slow_release_ms_);
    }

    EnvelopeFollowerT<SampleType> fast_{};
    EnvelopeFollowerT<SampleType> slow_{};
    SampleType attack_db_ = SampleType{0};
    SampleType sustain_db_ = SampleType{0};
    SampleType fast_attack_ms_ = SampleType{0.5};
    SampleType fast_release_ms_ = SampleType{20};
    SampleType slow_attack_ms_ = SampleType{15};
    SampleType slow_release_ms_ = SampleType{120};
    SampleType contrast_ = SampleType{0};
    SampleType gain_db_ = SampleType{0};
};

using TransientDesigner = TransientDesignerT<float>;
using TransientDesigner64 = TransientDesignerT<double>;

} // namespace pulp::signal

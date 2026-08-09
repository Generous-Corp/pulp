#pragma once

/// @file explicit_q_resonator_bank.hpp
/// Fixed-capacity bank of independently tuned band-pass resonators.
///
/// RT contract: prepare(), stage_band(), and publish() are control-thread
/// operations. After prepare(), process(), and reset()
/// allocate no memory, take no locks, perform no I/O, and throw no exceptions.
/// Complete recipes cross to the audio thread through a three-slot latest-value
/// publication and are adopted only at sample boundaries.

#include <pulp/runtime/triple_buffer.hpp>
#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

namespace pulp::signal {

template <typename SampleType = float> class ExplicitQResonatorBankT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    static constexpr int kMaximumBands = 64;

    struct BandRecipe {
        double frequency_hz = 1000.0;
        double q = 4.0;
        double gain_db = 0.0;
        double attack_ms = 5.0;
        double release_ms = 50.0;
    };

    [[nodiscard]] bool prepare(double sample_rate, int capacity) {
        if (!std::isfinite(sample_rate) || sample_rate < 20.0 / 0.45 || capacity < 1 ||
            capacity > kMaximumBands)
            return false;

        auto candidate = std::make_unique<PreparedState>();
        candidate->sample_rate = sample_rate;
        candidate->capacity = capacity;
        const double initial_frequency = std::min(1000.0, 0.45 * sample_rate);
        for (int i = 0; i < capacity; ++i) {
            auto& band = candidate->bands[static_cast<std::size_t>(i)];
            band.current.frequency_hz = initial_frequency;
            band.target.frequency_hz = initial_frequency;
            band.configured_frequency_hz = initial_frequency;
            band.filter.configure(Svf64::Mode::bandpass, sample_rate, initial_frequency, 4.0);
            band.envelope.prepare(sample_rate);
            band.envelope.set_attack_ms(5.0);
            band.envelope.set_release_ms(50.0);
        }
        state_ = std::move(candidate);
        staged_ = {};
        next_generation_ = 1;
        return true;
    }

    [[nodiscard]] bool stage_band(int index, const BandRecipe& recipe) noexcept {
        if (!state_ || index < 0 || index >= state_->capacity)
            return false;
        staged_[static_cast<std::size_t>(index)] = recipe;
        return true;
    }

    [[nodiscard]] bool set_coefficient_transition_ms(double transition_ms) noexcept {
        if (!valid_transition(transition_ms))
            return false;
        transition_ms_ = transition_ms;
        return true;
    }

    [[nodiscard]] bool publish(int active_count) noexcept {
        return publish(active_count, transition_ms_);
    }

    [[nodiscard]] bool publish(int active_count, double transition_ms) noexcept {
        if (!state_ || active_count < 0 || active_count > state_->capacity ||
            !valid_transition(transition_ms))
            return false;
        for (int i = 0; i < active_count; ++i)
            if (!valid_recipe(staged_[static_cast<std::size_t>(i)], state_->sample_rate))
                return false;
        const double transition_samples_exact = transition_ms * state_->sample_rate / 1000.0;
        if (!std::isfinite(transition_samples_exact) ||
            transition_samples_exact >=
                static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
            return false;

        PublishedRecipe payload;
        payload.bands = staged_;
        payload.active_count = active_count;
        payload.transition_samples =
            static_cast<std::uint64_t>(std::ceil(transition_samples_exact));
        payload.generation = next_generation_++;
        if (next_generation_ == 0)
            ++next_generation_;
        state_->publication.write(payload);
        return true;
    }

    SampleType process(SampleType input) noexcept {
        if (!state_)
            return SampleType{};
        adopt_publication();
        if (!std::isfinite(static_cast<double>(input))) {
            reset_active_audio_state();
            return SampleType{};
        }

        double sum = 0.0;
        for (int i = 0; i < state_->active_count; ++i) {
            auto& band = state_->bands[static_cast<std::size_t>(i)];
            advance(band);
            if (band.configured_attack_ms != band.current.attack_ms) {
                band.envelope.set_attack_ms(band.current.attack_ms);
                band.configured_attack_ms = band.current.attack_ms;
            }
            if (band.configured_release_ms != band.current.release_ms) {
                band.envelope.set_release_ms(band.current.release_ms);
                band.configured_release_ms = band.current.release_ms;
            }
            if (band.configured_frequency_hz != band.current.frequency_hz ||
                band.configured_q != band.current.q) {
                band.filter.configure(Svf64::Mode::bandpass, state_->sample_rate,
                                      band.current.frequency_hz, band.current.q);
                band.configured_frequency_hz = band.current.frequency_hz;
                band.configured_q = band.current.q;
            }
            const double filtered = band.filter.process(static_cast<double>(input));
            const double level = band.envelope.process(std::abs(filtered));
            const double output = filtered * db_to_gain(band.current.gain_db);
            band.last_output = std::isfinite(filtered) ? filtered : 0.0;
            band.last_envelope = std::isfinite(level) ? level : 0.0;
            if (!std::isfinite(output) || !std::isfinite(level)) {
                band.filter.reset();
                band.envelope.reset();
            }
            sum += std::isfinite(output) ? output : 0.0;
        }
        if (!std::isfinite(sum))
            return SampleType{};
        return to_sample(sum);
    }

    void process(const SampleType* input, SampleType* output, int num_samples) noexcept {
        if (!output || num_samples <= 0)
            return;
        for (int i = 0; i < num_samples; ++i)
            output[i] = process(input ? input[i] : SampleType{});
    }

    void reset() noexcept {
        if (!state_)
            return;
        adopt_publication();
        reset_active_audio_state();
        for (int i = 0; i < state_->active_count; ++i) {
            auto& band = state_->bands[static_cast<std::size_t>(i)];
            band.current = band.target;
            band.remaining = 0;
        }
    }

    [[nodiscard]] int capacity() const noexcept {
        return state_ ? state_->capacity : 0;
    }
    [[nodiscard]] int active_band_count() const noexcept {
        return state_ ? state_->active_count : 0;
    }
    [[nodiscard]] int latency_samples() const noexcept {
        return 0;
    }
    [[nodiscard]] std::size_t retained_bytes() const noexcept {
        return state_ ? sizeof(PreparedState) : 0;
    }
    /// Last filtered sample before gain_db is applied to the summed output.
    [[nodiscard]] SampleType band_output(int index) const noexcept {
        return valid_active_index(index)
                   ? to_sample(state_->bands[static_cast<std::size_t>(index)].last_output)
                   : SampleType{};
    }
    [[nodiscard]] SampleType envelope_at(int index) const noexcept {
        return valid_active_index(index)
                   ? to_sample(state_->bands[static_cast<std::size_t>(index)].last_envelope)
                   : SampleType{};
    }
    /// Current RT-side recipe, including any in-flight transition.
    [[nodiscard]] BandRecipe applied_band_recipe(int index) const noexcept {
        return state_ && index >= 0 && index < state_->capacity
                   ? state_->bands[static_cast<std::size_t>(index)].current
                   : BandRecipe{};
    }
    /// Ideal second-order -60 dB amplitude-tail estimate used for render sizing.
    [[nodiscard]] static double estimated_t60_seconds(const BandRecipe& recipe) noexcept {
        return valid_tail_recipe(recipe)
                   ? 6.907755278982137 * recipe.q / (3.14159265358979323846 * recipe.frequency_hz)
                   : 0.0;
    }

  private:
    struct PublishedRecipe {
        std::array<BandRecipe, kMaximumBands> bands{};
        int active_count = 0;
        std::uint64_t transition_samples = 0;
        std::uint64_t generation = 0;
    };
    struct BandState {
        Svf64 filter;
        BallisticsFilterT<double> envelope;
        BandRecipe current{};
        BandRecipe target{};
        BandRecipe step{};
        std::uint64_t remaining = 0;
        double last_output = 0.0;
        double last_envelope = 0.0;
        double configured_attack_ms = 5.0;
        double configured_release_ms = 50.0;
        double configured_frequency_hz = 1000.0;
        double configured_q = 4.0;
    };
    struct PreparedState {
        double sample_rate = 0.0;
        int capacity = 0;
        int active_count = 0;
        std::uint64_t consumed_generation = 0;
        runtime::TripleBuffer<PublishedRecipe> publication;
        std::array<BandState, kMaximumBands> bands{};
    };

    static bool valid_recipe(const BandRecipe& r, double sample_rate) noexcept {
        return std::isfinite(r.frequency_hz) && r.frequency_hz >= 20.0 &&
               r.frequency_hz <= 0.45 * sample_rate && std::isfinite(r.q) && r.q >= 0.5 &&
               r.q <= 100.0 && std::isfinite(r.gain_db) && r.gain_db >= -60.0 &&
               r.gain_db <= 24.0 && std::isfinite(r.attack_ms) && r.attack_ms >= 0.01 &&
               r.attack_ms <= 60000.0 && std::isfinite(r.release_ms) && r.release_ms >= 0.01 &&
               r.release_ms <= 60000.0;
    }
    static bool valid_transition(double ms) noexcept {
        return std::isfinite(ms) && ms >= 0.0 && ms <= 500.0;
    }
    static bool valid_tail_recipe(const BandRecipe& recipe) noexcept {
        return std::isfinite(recipe.frequency_hz) && recipe.frequency_hz > 0.0 &&
               std::isfinite(recipe.q) && recipe.q > 0.0;
    }
    static double db_to_gain(double db) noexcept {
        return std::pow(10.0, db / 20.0);
    }
    static SampleType to_sample(double value) noexcept {
        if (!std::isfinite(value))
            return SampleType{};
        const double limit = static_cast<double>(std::numeric_limits<SampleType>::max());
        return static_cast<SampleType>(std::clamp(value, -limit, limit));
    }
    static BandRecipe delta(const BandRecipe& a, const BandRecipe& b, double n) noexcept {
        return {(b.frequency_hz - a.frequency_hz) / n, (b.q - a.q) / n, (b.gain_db - a.gain_db) / n,
                (b.attack_ms - a.attack_ms) / n, (b.release_ms - a.release_ms) / n};
    }
    static void add(BandRecipe& a, const BandRecipe& d) noexcept {
        a.frequency_hz += d.frequency_hz;
        a.q += d.q;
        a.gain_db += d.gain_db;
        a.attack_ms += d.attack_ms;
        a.release_ms += d.release_ms;
    }
    void adopt_publication() noexcept {
        const auto& p = state_->publication.read();
        if (p.generation == 0 || p.generation == state_->consumed_generation)
            return;
        const int touched_bands = std::max(state_->active_count, p.active_count);
        for (int i = 0; i < touched_bands; ++i) {
            auto& band = state_->bands[static_cast<std::size_t>(i)];
            if (i >= p.active_count) {
                band.filter.reset();
                band.envelope.reset();
                band.last_output = 0.0;
                band.last_envelope = 0.0;
                band.remaining = 0;
                continue;
            }
            band.target = p.bands[static_cast<std::size_t>(i)];
            if (p.transition_samples == 0) {
                band.current = band.target;
                band.remaining = 0;
            } else {
                band.step =
                    delta(band.current, band.target, static_cast<double>(p.transition_samples));
                band.remaining = p.transition_samples;
            }
        }
        state_->active_count = p.active_count;
        state_->consumed_generation = p.generation;
    }
    static void advance(BandState& band) noexcept {
        if (band.remaining == 0)
            return;
        add(band.current, band.step);
        if (--band.remaining == 0)
            band.current = band.target;
    }
    void reset_active_audio_state() noexcept {
        for (int i = 0; i < state_->active_count; ++i) {
            auto& band = state_->bands[static_cast<std::size_t>(i)];
            band.filter.reset();
            band.envelope.reset();
            band.last_output = 0.0;
            band.last_envelope = 0.0;
        }
    }
    bool valid_active_index(int index) const noexcept {
        return state_ && index >= 0 && index < state_->active_count;
    }

    std::unique_ptr<PreparedState> state_;
    std::array<BandRecipe, kMaximumBands> staged_{};
    double transition_ms_ = 20.0;
    std::uint64_t next_generation_ = 1;
};

using ExplicitQResonatorBank = ExplicitQResonatorBankT<float>;
using ExplicitQResonatorBank64 = ExplicitQResonatorBankT<double>;

} // namespace pulp::signal

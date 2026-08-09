#pragma once

/// @file formant_filter_bank.hpp
/// Fixed-capacity vowel/formant filter with transactional whole-bank retuning.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/frequency_response.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class FormantVowel { a, e, i, o, u };

enum class FormantConfigureStatus {
    configured,
    not_prepared,
    invalid_count,
    non_finite,
    out_of_range,
    unordered,
    unstable,
};

/// A parallel bank of resonant band-pass sections for fixed source-filter colour.
///
/// This is an audio filter, not a formant tracker and not an STFT envelope
/// shifter. Frequencies and bandwidths are supplied explicitly. A complete
/// recipe is validated before any active or requested state changes. Retunes
/// crossfade two complete stable banks; recursive coefficients are never
/// interpolated.
///
/// Headroom policy: each band's linear weight is divided by the sum of all
/// absolute weights and by the requested headroom gain. Since an RBJ band-pass
/// section has at most unity steady-state magnitude, the summed response is
/// bounded by that headroom gain. This intentionally normalizes level rather
/// than adding a limiter or nonlinear recovery stage.
///
/// RT contract: all storage is fixed. prepare(), configure(), process(), reset(),
/// and response inspection allocate no memory, take no locks, perform no I/O,
/// and throw no exceptions. Configure at a block boundary. One instance owns
/// one recursive signal path; use one instance per channel.
template <typename SampleType = float, std::size_t MaxFormants = 5> class FormantFilterBankT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxFormants >= 1 && MaxFormants <= 8);

  public:
    static constexpr std::size_t storage_capacity = MaxFormants;
    static constexpr double min_frequency_hz = 40.0;
    static constexpr double min_bandwidth_hz = 20.0;
    static constexpr double min_q = 0.5;
    static constexpr double max_q = 50.0;
    static constexpr double min_gain_db = -60.0;
    static constexpr double max_gain_db = 24.0;
    static constexpr double min_headroom_db = 0.0;
    static constexpr double max_headroom_db = 24.0;
    static constexpr std::size_t default_transition_samples = 64;

    struct FormantSpec {
        double frequency_hz = 1000.0;
        double bandwidth_hz = 100.0;
        double gain_db = 0.0;
    };

    struct Recipe {
        std::array<FormantSpec, MaxFormants> formants{};
        std::size_t count = 0;
        double headroom_db = 3.0;
    };

    bool prepare(double sample_rate, std::size_t capacity = MaxFormants) noexcept {
        const SampleType processing_rate = static_cast<SampleType>(sample_rate);
        if (!std::isfinite(sample_rate) || !std::isfinite(processing_rate) ||
            processing_rate < static_cast<SampleType>(min_frequency_hz / 0.45) || capacity == 0 ||
            capacity > MaxFormants)
            return false;
        sample_rate_ = static_cast<double>(processing_rate);
        capacity_ = capacity;
        active_ = {};
        transition_ = {};
        requested_ = {};
        requested_coefficients_ = {};
        requested_weights_ = {};
        requested_tail_seconds_ = 0.0;
        queued_ = {};
        transition_total_ = 0;
        transition_remaining_ = 0;
        queued_transition_samples_ = 0;
        queued_transition_ = false;
        prepared_ = true;
        return true;
    }

    bool prepared() const noexcept {
        return prepared_;
    }
    std::size_t capacity() const noexcept {
        return capacity_;
    }
    double sample_rate() const noexcept {
        return sample_rate_;
    }

    void set_transition_samples(std::size_t samples) noexcept {
        transition_samples_ = samples;
    }
    std::size_t transition_samples() const noexcept {
        return transition_samples_;
    }
    bool transition_active() const noexcept {
        return transition_remaining_ != 0;
    }

    FormantConfigureStatus configure(std::span<const FormantSpec> formants,
                                     double headroom_db = 3.0) noexcept {
        if (!prepared_)
            return FormantConfigureStatus::not_prepared;
        const auto status = validate(formants, headroom_db);
        if (status != FormantConfigureStatus::configured)
            return status;

        Recipe candidate;
        candidate.count = formants.size();
        candidate.headroom_db = headroom_db;
        std::copy(formants.begin(), formants.end(), candidate.formants.begin());
        if (recipes_equal(candidate, requested_))
            return FormantConfigureStatus::configured;
        if (transition_active() && recipes_equal(candidate, transition_.recipe)) {
            requested_ = candidate;
            capture_requested(transition_);
            queued_ = {};
            queued_transition_samples_ = 0;
            queued_transition_ = false;
            return FormantConfigureStatus::configured;
        }
        Bank designed;
        if (!design(candidate, designed))
            return FormantConfigureStatus::unstable;

        requested_ = candidate;
        capture_requested(designed);
        if (!transition_active()) {
            if (transition_samples_ == 0) {
                load_coefficients(designed, active_, true);
            } else {
                transition_ = active_;
                load_coefficients(designed, transition_, true);
                transition_total_ = transition_samples_;
                transition_remaining_ = transition_samples_;
            }
        } else if (transition_remaining_ == transition_total_) {
            load_coefficients(designed, transition_, true);
        } else {
            queued_ = designed;
            queued_transition_samples_ =
                transition_samples_ != 0 ? transition_samples_ : transition_total_;
            queued_transition_ = true;
        }
        return FormantConfigureStatus::configured;
    }

    FormantConfigureStatus configure(const Recipe& recipe) noexcept {
        if (recipe.count > MaxFormants)
            return prepared_ ? FormantConfigureStatus::invalid_count
                             : FormantConfigureStatus::not_prepared;
        return configure(std::span<const FormantSpec>(recipe.formants.data(), recipe.count),
                         recipe.headroom_db);
    }

    FormantConfigureStatus configure_vowel_morph(FormantVowel from, FormantVowel to, double amount,
                                                 double headroom_db = 3.0) noexcept {
        if (!std::isfinite(amount))
            return FormantConfigureStatus::non_finite;
        if (amount < 0.0 || amount > 1.0)
            return FormantConfigureStatus::out_of_range;
        Recipe recipe = interpolate(vowel_recipe(from), vowel_recipe(to), amount);
        recipe.headroom_db = headroom_db;
        return configure(recipe);
    }

    const Recipe& requested_recipe() const noexcept {
        return requested_;
    }

    SampleType process(SampleType input) noexcept {
        if (!prepared_)
            return input;
        if (!std::isfinite(static_cast<double>(input))) {
            reset();
            return SampleType{};
        }

        double output = 0.0;
        if (!transition_active()) {
            output = process_bank(active_, static_cast<double>(input));
        } else {
            const double from = process_bank(active_, static_cast<double>(input));
            const double to = process_bank(transition_, static_cast<double>(input));
            const std::size_t completed = transition_total_ - transition_remaining_ + 1;
            const double mix =
                static_cast<double>(completed) / static_cast<double>(transition_total_);
            output = from + (to - from) * mix;
            if (--transition_remaining_ == 0) {
                active_ = transition_;
                if (queued_transition_) {
                    transition_ = active_;
                    load_coefficients(queued_, transition_, true);
                    queued_transition_ = false;
                    transition_total_ = queued_transition_samples_;
                    transition_remaining_ = transition_total_;
                    queued_transition_samples_ = 0;
                }
            }
        }
        if (!std::isfinite(output)) {
            reset();
            return SampleType{};
        }
        const double limit = static_cast<double>(std::numeric_limits<SampleType>::max());
        return static_cast<SampleType>(std::clamp(output, -limit, limit));
    }

    bool process_block(const SampleType* input, SampleType* output, std::size_t frames) noexcept {
        if ((input == nullptr || output == nullptr) && frames != 0)
            return false;
        for (std::size_t i = 0; i < frames; ++i)
            output[i] = process(input[i]);
        return true;
    }

    bool process_block(SampleType* samples, std::size_t frames) noexcept {
        return process_block(samples, samples, frames);
    }

    void reset() noexcept {
        reset_bank(active_);
        reset_bank(transition_);
        reset_bank(queued_);
    }

    static constexpr int latency_samples() noexcept {
        return 0;
    }

    double tail_seconds() const noexcept {
        double result = requested_tail_seconds_;
        if (transition_active()) {
            result = std::max(result, tail_of(active_));
            result = std::max(result, tail_of(transition_));
        }
        return result;
    }

    BiquadCoefficientsT<SampleType> coefficients(std::size_t index) const noexcept {
        return index < requested_.count ? requested_coefficients_[index]
                                        : BiquadCoefficientsT<SampleType>{};
    }

    /// Exact complex-sum magnitude of the requested endpoint.
    double magnitude(double frequency_hz) const noexcept {
        if (!prepared_ || requested_.count == 0)
            return 1.0;
        const double omega =
            angular_frequency(std::isfinite(frequency_hz) ? frequency_hz : 0.0, sample_rate_);
        const std::complex<double> z1 = std::polar(1.0, -omega);
        const std::complex<double> z2 = z1 * z1;
        std::complex<double> sum{};
        for (std::size_t i = 0; i < requested_.count; ++i) {
            const auto& c = requested_coefficients_[i];
            const std::complex<double> numerator = static_cast<double>(c.b0) +
                                                   static_cast<double>(c.b1) * z1 +
                                                   static_cast<double>(c.b2) * z2;
            const std::complex<double> denominator =
                1.0 + static_cast<double>(c.a1) * z1 + static_cast<double>(c.a2) * z2;
            const double denominator_magnitude = std::abs(denominator);
            if (!(denominator_magnitude > 0.0))
                return 0.0;
            sum += requested_weights_[i] * numerator / denominator;
        }
        return std::abs(sum);
    }

    double magnitude_db(double frequency_hz) const noexcept {
        return static_cast<double>(magnitude_to_db(magnitude(frequency_hz)));
    }

    void response_curve_db(double min_hz, double max_hz,
                           std::span<SampleType> output) const noexcept {
        if (output.empty())
            return;
        if (!prepared_) {
            std::fill(output.begin(), output.end(), SampleType{});
            return;
        }
        const double nyquist = sample_rate_ * 0.5;
        const double lo = std::clamp(std::isfinite(min_hz) ? min_hz : min_frequency_hz,
                                     min_frequency_hz, nyquist);
        const double clamped_hi =
            std::clamp(std::isfinite(max_hz) ? max_hz : lo, min_frequency_hz, nyquist);
        const double hi = std::max(lo, clamped_hi);
        for (std::size_t i = 0; i < output.size(); ++i) {
            const double t = output.size() == 1
                                 ? 0.0
                                 : static_cast<double>(i) / static_cast<double>(output.size() - 1);
            const double hz = std::exp(std::log(lo) + t * (std::log(hi) - std::log(lo)));
            output[i] = static_cast<SampleType>(magnitude_db(hz));
        }
    }

    static constexpr Recipe vowel_recipe(FormantVowel vowel) noexcept {
        switch (vowel) {
        case FormantVowel::a:
            return make_vowel({800, 1150, 2900, 3900, 4950}, {80, 90, 120, 130, 140},
                              {0, -4, -9, -12, -15});
        case FormantVowel::e:
            return make_vowel({400, 1600, 2700, 3300, 4950}, {70, 100, 120, 150, 200},
                              {0, -4, -8, -12, -15});
        case FormantVowel::i:
            return make_vowel({350, 1700, 2700, 3700, 4950}, {60, 100, 120, 150, 200},
                              {0, -3, -9, -12, -15});
        case FormantVowel::o:
            return make_vowel({450, 800, 2830, 3800, 4950}, {70, 80, 100, 130, 150},
                              {0, -5, -10, -13, -16});
        case FormantVowel::u:
            return make_vowel({325, 700, 2530, 3500, 4950}, {50, 60, 170, 180, 200},
                              {0, -6, -12, -15, -18});
        }
        return {};
    }

    static Recipe interpolate(const Recipe& from, const Recipe& to, double amount) noexcept {
        Recipe result;
        if (!std::isfinite(amount))
            return result;
        amount = std::clamp(amount, 0.0, 1.0);
        result.count = std::min({from.count, to.count, MaxFormants});
        result.headroom_db = from.headroom_db + (to.headroom_db - from.headroom_db) * amount;
        for (std::size_t i = 0; i < result.count; ++i) {
            const auto& a = from.formants[i];
            const auto& b = to.formants[i];
            result.formants[i].frequency_hz =
                std::exp(std::log(a.frequency_hz) +
                         (std::log(b.frequency_hz) - std::log(a.frequency_hz)) * amount);
            result.formants[i].bandwidth_hz =
                a.bandwidth_hz + (b.bandwidth_hz - a.bandwidth_hz) * amount;
            result.formants[i].gain_db = a.gain_db + (b.gain_db - a.gain_db) * amount;
        }
        return result;
    }

  private:
    struct Bank {
        std::array<BiquadT<SampleType>, MaxFormants> filters{};
        std::array<double, MaxFormants> weights{};
        Recipe recipe{};
    };

    static constexpr Recipe make_vowel(const std::array<double, 5>& frequencies,
                                       const std::array<double, 5>& bandwidths,
                                       const std::array<double, 5>& gains) noexcept {
        Recipe result;
        result.count = std::min<std::size_t>(5, MaxFormants);
        for (std::size_t i = 0; i < result.count; ++i)
            result.formants[i] = {frequencies[i], bandwidths[i], gains[i]};
        return result;
    }

    static bool recipes_equal(const Recipe& lhs, const Recipe& rhs) noexcept {
        if (lhs.count != rhs.count || lhs.headroom_db != rhs.headroom_db)
            return false;
        for (std::size_t i = 0; i < lhs.count; ++i) {
            if (lhs.formants[i].frequency_hz != rhs.formants[i].frequency_hz ||
                lhs.formants[i].bandwidth_hz != rhs.formants[i].bandwidth_hz ||
                lhs.formants[i].gain_db != rhs.formants[i].gain_db)
                return false;
        }
        return true;
    }

    FormantConfigureStatus validate(std::span<const FormantSpec> formants,
                                    double headroom_db) const noexcept {
        if (formants.empty() || formants.size() > capacity_)
            return FormantConfigureStatus::invalid_count;
        if (!std::isfinite(headroom_db))
            return FormantConfigureStatus::non_finite;
        if (headroom_db < min_headroom_db || headroom_db > max_headroom_db)
            return FormantConfigureStatus::out_of_range;
        double previous = 0.0;
        for (const auto& formant : formants) {
            if (!std::isfinite(formant.frequency_hz) || !std::isfinite(formant.bandwidth_hz) ||
                !std::isfinite(formant.gain_db))
                return FormantConfigureStatus::non_finite;
            const double q = formant.frequency_hz / formant.bandwidth_hz;
            if (formant.frequency_hz < min_frequency_hz ||
                formant.frequency_hz > 0.45 * sample_rate_ ||
                formant.bandwidth_hz < min_bandwidth_hz || q < min_q || q > max_q ||
                formant.gain_db < min_gain_db || formant.gain_db > max_gain_db)
                return FormantConfigureStatus::out_of_range;
            if (formant.frequency_hz <= previous)
                return FormantConfigureStatus::unordered;
            previous = formant.frequency_hz;
        }
        return FormantConfigureStatus::configured;
    }

    bool design(const Recipe& recipe, Bank& bank) noexcept {
        bank = {};
        bank.recipe = recipe;
        double weight_sum = 0.0;
        for (std::size_t i = 0; i < recipe.count; ++i)
            weight_sum += std::pow(10.0, recipe.formants[i].gain_db / 20.0);
        const double headroom_gain = std::pow(10.0, -recipe.headroom_db / 20.0);
        for (std::size_t i = 0; i < recipe.count; ++i) {
            const auto& formant = recipe.formants[i];
            BiquadT<SampleType> designer;
            designer.set_coefficients(
                BiquadT<SampleType>::Type::bandpass, static_cast<SampleType>(formant.frequency_hz),
                static_cast<SampleType>(formant.frequency_hz / formant.bandwidth_hz),
                static_cast<SampleType>(sample_rate_));
            const auto coefficients = designer.coefficients();
            if (!finite(coefficients) || !biquad_is_stable(coefficients))
                return false;
            bank.filters[i].set_coefficients(coefficients);
            bank.weights[i] = headroom_gain * std::pow(10.0, formant.gain_db / 20.0) / weight_sum;
        }
        return true;
    }

    static bool finite(const BiquadCoefficientsT<SampleType>& coefficients) noexcept {
        return std::isfinite(coefficients.b0) && std::isfinite(coefficients.b1) &&
               std::isfinite(coefficients.b2) && std::isfinite(coefficients.a1) &&
               std::isfinite(coefficients.a2);
    }

    void load_coefficients(const Bank& source, Bank& destination, bool preserve_state) noexcept {
        if (!preserve_state)
            reset_bank(destination);
        destination.recipe = source.recipe;
        destination.weights = source.weights;
        for (std::size_t i = 0; i < MaxFormants; ++i) {
            destination.filters[i].set_coefficients(source.filters[i].coefficients());
            if (i >= source.recipe.count)
                destination.filters[i].reset();
        }
    }

    void capture_requested(const Bank& source) noexcept {
        requested_coefficients_ = {};
        requested_weights_ = {};
        for (std::size_t i = 0; i < source.recipe.count; ++i) {
            const auto c = source.filters[i].coefficients();
            requested_coefficients_[i] = c;
            requested_weights_[i] = source.weights[i];
        }
        requested_tail_seconds_ = tail_of(source);
    }

    static double process_bank(Bank& bank, double input) noexcept {
        if (bank.recipe.count == 0)
            return input;
        double sum = 0.0;
        for (std::size_t i = 0; i < bank.recipe.count; ++i)
            sum += bank.weights[i] * bank.filters[i].process(input);
        return sum;
    }

    static void reset_bank(Bank& bank) noexcept {
        for (auto& filter : bank.filters)
            filter.reset();
    }

    double tail_of(const Bank& bank) const noexcept {
        double result = 0.0;
        for (std::size_t i = 0; i < bank.recipe.count; ++i) {
            const auto c = bank.filters[i].coefficients();
            const std::complex<double> discriminant = std::sqrt(
                std::complex<double>{static_cast<double>(c.a1) * static_cast<double>(c.a1) -
                                         4.0 * static_cast<double>(c.a2),
                                     0.0});
            const double radius =
                std::max(std::abs((-static_cast<double>(c.a1) + discriminant) * 0.5),
                         std::abs((-static_cast<double>(c.a1) - discriminant) * 0.5));
            if (radius > 0.0 && radius < 1.0)
                result = std::max(result, -std::log(0.001) / (-std::log(radius) * sample_rate_));
        }
        return result;
    }

    double sample_rate_ = 0.0;
    std::size_t capacity_ = 0;
    std::size_t transition_samples_ = default_transition_samples;
    std::size_t transition_total_ = 0;
    std::size_t transition_remaining_ = 0;
    std::size_t queued_transition_samples_ = 0;
    bool prepared_ = false;
    bool queued_transition_ = false;
    Bank active_{};
    Bank transition_{};
    Bank queued_{};
    Recipe requested_{};
    std::array<BiquadCoefficientsT<SampleType>, MaxFormants> requested_coefficients_{};
    std::array<double, MaxFormants> requested_weights_{};
    double requested_tail_seconds_ = 0.0;
};

using FormantFilterBank = FormantFilterBankT<float>;
using FormantFilterBank64 = FormantFilterBankT<double>;

} // namespace pulp::signal

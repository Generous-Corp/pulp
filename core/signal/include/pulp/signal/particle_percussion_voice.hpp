#pragma once

#include <pulp/signal/modal_bank.hpp>
#include <pulp/signal/particle_collision_exciter.hpp>
#include <pulp/signal/two_pole_resonator.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

/// Single particle-percussion voice composed from the shared collision model
/// and Pulp's existing resonators. This type owns no graph or instrument-level
/// voice allocation.
///
/// `prepare()` may allocate through ModalBankT. Parameter setters and
/// `set_model()` are control-side operations. `process()`, `excite()`, and
/// `reset()` are bounded and allocation-free after preparation.
template <typename SampleType = float>
class ParticlePercussionVoiceT {
    static_assert(std::is_floating_point_v<SampleType>);

public:
    enum class Model : std::uint8_t { shaker, maraca, rattle, tambourine };

    static constexpr std::size_t kMaxBands = 4;
    static constexpr std::size_t kTambourineModes = 5;
    static constexpr double kMinSupportedSampleRate =
        ParticleCollisionExciterT<SampleType>::kMinSupportedSampleRate;

    ParticlePercussionVoiceT() {
        configure_model();
        apply_routing_seed();
        reset();
    }

    /// Prepares at 8 kHz or above. Invalid, non-finite, or lower rates retain
    /// the prior valid rate; every call consistently resets dynamic state.
    void prepare(double sample_rate) {
        if (std::isfinite(sample_rate) && sample_rate >= kMinSupportedSampleRate)
            sample_rate_ = sample_rate;
        exciter_.prepare(sample_rate_);
        modal_bank_.prepare(sample_rate_, static_cast<int>(kTambourineModes), 1);
        prepared_ = true;
        configure_model();
        reset();
    }

    /// Changes the body recipe and clears the voice's exciter and resonators.
    void set_model(Model model) {
        switch (model) {
        case Model::shaker:
        case Model::maraca:
        case Model::rattle:
        case Model::tambourine:
            model_ = model;
            break;
        default:
            return;
        }
        configure_model();
        reset();
    }

    void set_seed(std::uint64_t seed) noexcept {
        seed_ = seed;
        exciter_.set_seed(seed_);
        apply_routing_seed();
        reset();
    }

    void set_particle_count(int count) noexcept { exciter_.set_particle_count(count); }
    void set_event_rate(SampleType rate) noexcept { exciter_.set_event_rate(rate); }
    void set_energy_decay(SampleType milliseconds) noexcept {
        exciter_.set_energy_decay(milliseconds);
    }
    void set_sustain_floor(SampleType floor) noexcept { exciter_.set_sustain_floor(floor); }
    void set_collision_energy_loss(SampleType loss) noexcept {
        exciter_.set_collision_energy_loss(loss);
    }

    void reset() noexcept {
        exciter_.reset();
        routing_rng_.reset();
        for (auto& resonator : resonators_)
            resonator.reset();
        modal_bank_.reset();
    }

    void excite(SampleType amount01) noexcept { exciter_.excite(amount01); }

    SampleType process() noexcept {
        const auto event = exciter_.next();
        if (model_ == Model::tambourine) {
            const SampleType input = event.occurred ? event.amplitude : SampleType{};
            SampleType output{};
            modal_bank_.process_add(&input, &output, 1);
            return std::isfinite(static_cast<double>(output)) ? output : SampleType{};
        }

        std::size_t selected = kMaxBands;
        if (event.occurred)
            selected = select_band();

        SampleType output{};
        for (std::size_t band = 0; band < kMaxBands; ++band) {
            const SampleType input = selected == band
                                         ? event.amplitude * bands_[band].gain
                                         : SampleType{};
            output += resonators_[band].process(input);
        }
        return std::isfinite(static_cast<double>(output)) ? output : SampleType{};
    }

    void process(SampleType* output, int num_samples) noexcept {
        if (output == nullptr || num_samples <= 0)
            return;
        for (int sample = 0; sample < num_samples; ++sample)
            output[sample] = process();
    }

    [[nodiscard]] Model model() const noexcept { return model_; }
    [[nodiscard]] double sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] bool prepared() const noexcept { return prepared_; }

    static constexpr int latency_samples() noexcept { return 0; }

    /// Heap storage retained by the prepared five-mode ModalBankT. Its seven
    /// aligned buffers each round one 16-lane group to the SIMD alignment.
    [[nodiscard]] std::size_t retained_bytes() const noexcept {
        if (!prepared_)
            return 0;
        constexpr std::size_t bytes_per_buffer =
            ((ModalBankT<SampleType>::kLanes * sizeof(SampleType) + kSimdAlignment - 1) /
             kSimdAlignment) * kSimdAlignment;
        return 7u * bytes_per_buffer;
    }

    /// Conservative render tail: remaining collision-envelope settling plus
    /// six T60 intervals for the longest configured body resonance.
    [[nodiscard]] int tail_samples() const noexcept {
        const int exciter_tail = exciter_.tail_samples();
        if (exciter_tail < 0)
            return -1;
        const double body_samples = std::ceil(6.0 * longest_body_t60_seconds_ * sample_rate_);
        const double total = static_cast<double>(exciter_tail) + body_samples;
        return total >= static_cast<double>(std::numeric_limits<int>::max())
                   ? std::numeric_limits<int>::max()
                   : static_cast<int>(total);
    }

private:
    struct Band {
        double frequency_hz;
        double q;
        SampleType gain;
        double weight;
    };

    static constexpr std::array<Band, kMaxBands> kShaker{{
        {2200.0, 1.5, SampleType{1.0}, 0.35},
        {4200.0, 2.0, SampleType{0.8}, 0.30},
        {6800.0, 1.8, SampleType{0.6}, 0.20},
        {9500.0, 1.2, SampleType{0.4}, 0.15},
    }};
    static constexpr std::array<Band, kMaxBands> kMaraca{{
        {2900.0, 3.0, SampleType{1.0}, 0.40},
        {5100.0, 2.5, SampleType{0.5}, 0.25},
        {7600.0, 2.0, SampleType{0.3}, 0.20},
        {10400.0, 1.5, SampleType{0.2}, 0.15},
    }};
    static constexpr std::array<Band, kMaxBands> kRattle{{
        {550.0, 4.0, SampleType{1.0}, 0.30},
        {950.0, 3.5, SampleType{0.7}, 0.30},
        {1600.0, 3.0, SampleType{0.5}, 0.25},
        {2600.0, 2.5, SampleType{0.3}, 0.15},
    }};

    static constexpr std::array<ModalMode, kTambourineModes> kTambourine{{
        {3200.0f, 0.180f, 1.0f},
        {7584.0f, 0.140f, 0.7f},
        {12416.0f, 0.100f, 0.5f},
        {17952.0f, 0.070f, 0.35f},
        {23328.0f, 0.045f, 0.2f},
    }};

    void configure_model() {
        if (model_ == Model::tambourine) {
            modal_bank_.set_modes(std::span<const ModalMode>(kTambourine));
            longest_body_t60_seconds_ = 0.180;
            return;
        }

        switch (model_) {
        case Model::shaker: bands_ = kShaker; break;
        case Model::maraca: bands_ = kMaraca; break;
        case Model::rattle: bands_ = kRattle; break;
        case Model::tambourine: break;
        }

        longest_body_t60_seconds_ = 0.0;
        for (std::size_t band = 0; band < kMaxBands; ++band) {
            const double maximum_frequency = std::max(1.0, 0.499 * sample_rate_);
            const double effective_frequency =
                std::clamp(bands_[band].frequency_hz, 1.0, maximum_frequency);
            const double t60_ms = 1000.0 * 3.0 * std::log(10.0) * bands_[band].q /
                                  (3.14159265358979323846 * effective_frequency);
            resonators_[band].set_sample_rate(sample_rate_);
            resonators_[band].set_frequency(effective_frequency);
            resonators_[band].set_t60_ms(t60_ms);
            longest_body_t60_seconds_ = std::max(longest_body_t60_seconds_, 0.001 * t60_ms);
        }
    }

    std::size_t select_band() noexcept {
        const double draw = 0.5 * (static_cast<double>(routing_rng_.white()) + 1.0);
        double total = 0.0;
        for (const auto& band : bands_)
            total += band.weight;
        double cumulative = 0.0;
        for (std::size_t band = 0; band + 1 < kMaxBands; ++band) {
            cumulative += bands_[band].weight / total;
            if (draw < cumulative)
                return band;
        }
        return kMaxBands - 1;
    }

    void apply_routing_seed() noexcept {
        const std::uint64_t mixed = mix64(seed_, 0u, 0x524F5554494E47ull);
        const auto folded = static_cast<std::uint32_t>(mixed ^ (mixed >> 32));
        routing_rng_.set_seed(folded == 0u ? NoiseSourceT<double>::default_seed : folded);
    }

    ParticleCollisionExciterT<SampleType> exciter_;
    std::array<TwoPoleResonatorT<SampleType>, kMaxBands> resonators_;
    ModalBankT<SampleType> modal_bank_;
    NoiseSourceT<double> routing_rng_;
    std::array<Band, kMaxBands> bands_ = kShaker;
    double sample_rate_ = ParticleCollisionExciterT<SampleType>::kDefaultSampleRate;
    double longest_body_t60_seconds_ = 0.0;
    std::uint64_t seed_ = ParticleCollisionExciterT<SampleType>::kDefaultSeed;
    Model model_ = Model::shaker;
    bool prepared_ = false;
};

using ParticlePercussionVoice = ParticlePercussionVoiceT<float>;
using ParticlePercussionVoice64 = ParticlePercussionVoiceT<double>;

} // namespace pulp::signal

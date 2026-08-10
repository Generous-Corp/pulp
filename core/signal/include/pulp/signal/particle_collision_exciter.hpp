#pragma once

#include <pulp/signal/noise_source.hpp>
#include <pulp/signal/rng.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace pulp::signal {

/// Bounded stochastic collision model for shaken particle instruments.
///
/// One event draw is consumed on every sample. Amplitude jitter is drawn from
/// a separately seeded stream only when a collision occurs, so changing the
/// amplitude law cannot move event times. `reset()` rewinds both streams and
/// clears the accumulated energy.
///
/// RT contract: after control-side `prepare()` and parameter updates, `next()`,
/// `excite()`, and `reset()` allocate nothing, lock nothing, and perform no I/O.
template <typename SampleType = float>
class ParticleCollisionExciterT {
    static_assert(std::is_floating_point_v<SampleType>);

public:
    static constexpr int kMaxParticleCount = 2000;
    static constexpr double kDefaultSampleRate = 48000.0;
    static constexpr double kMinSupportedSampleRate = 8000.0;
    static constexpr std::uint64_t kDefaultSeed = 0x5041525449434C45ull;

    struct CollisionEvent {
        bool occurred = false;
        SampleType amplitude{};
    };

    ParticleCollisionExciterT() {
        apply_seed();
        update_decay_coefficient();
        reset();
    }

    /// Reconfigures the two NoiseSourceT streams and clears dynamic state.
    /// Rates below 8 kHz or non-finite rates retain the prior valid rate.
    /// NoiseSourceT::prepare() is bounded control-side work.
    void prepare(double sample_rate) {
        if (std::isfinite(sample_rate) && sample_rate >= kMinSupportedSampleRate)
            sample_rate_ = sample_rate;
        event_rng_.prepare(sample_rate_);
        jitter_rng_.prepare(sample_rate_);
        update_decay_coefficient();
        reset();
    }

    void set_seed(std::uint64_t seed) noexcept {
        seed_ = seed;
        apply_seed();
        reset();
    }

    void set_particle_count(int count) noexcept {
        particle_count_ = std::clamp(count, 1, kMaxParticleCount);
        particle_scale_ = std::sqrt(static_cast<double>(particle_count_));
    }

    void set_event_rate(SampleType events_per_second) noexcept {
        const double value = static_cast<double>(events_per_second);
        if (std::isfinite(value))
            event_rate_ = std::clamp(value, 1.0, 20000.0);
    }

    void set_energy_decay(SampleType milliseconds) noexcept {
        const double value = static_cast<double>(milliseconds);
        if (!std::isfinite(value))
            return;
        energy_decay_ms_ = std::clamp(value, 5.0, 4000.0);
        update_decay_coefficient();
    }

    void set_sustain_floor(SampleType floor) noexcept {
        const double value = static_cast<double>(floor);
        if (!std::isfinite(value))
            return;
        sustain_floor_ = std::clamp(value, 0.0, 0.2);
        snap_energy();
    }

    /// Absolute normalized-energy decrement applied after each collision.
    void set_collision_energy_loss(SampleType loss) noexcept {
        const double value = static_cast<double>(loss);
        if (std::isfinite(value))
            collision_energy_loss_ = std::clamp(value, 0.0001, 0.05);
    }

    void reset() noexcept {
        energy_ = 0.0;
        event_rng_.reset();
        jitter_rng_.reset();
    }

    void excite(SampleType amount01) noexcept {
        const double amount = static_cast<double>(amount01);
        if (std::isfinite(amount) && amount > 0.0)
            energy_ = std::clamp(energy_ + amount, 0.0, 1.0);
    }

    CollisionEvent next() noexcept {
        energy_ *= decay_coefficient_;
        snap_energy();

        // NoiseSourceT::white() is uniform on [-1, 1), hence this is [0, 1).
        const double event_draw = 0.5 * (static_cast<double>(event_rng_.white()) + 1.0);
        const double lambda_dt = event_rate_ * particle_scale_ * energy_ / sample_rate_;
        const double probability = -std::expm1(-lambda_dt);
        if (!(event_draw < probability))
            return {};

        const double pre_loss_energy = energy_;
        const double jitter = 0.5 * (static_cast<double>(jitter_rng_.white()) + 1.0);
        const double amplitude = pre_loss_energy * (0.5 + 0.5 * jitter);
        SampleType converted_amplitude = static_cast<SampleType>(amplitude);
        if (!(static_cast<double>(converted_amplitude) < pre_loss_energy)) {
            converted_amplitude = std::nextafter(
                static_cast<SampleType>(pre_loss_energy),
                std::numeric_limits<SampleType>::lowest());
        }

        energy_ = std::max(0.0, energy_ - collision_energy_loss_);
        snap_energy();
        return {true, converted_amplitude};
    }

    [[nodiscard]] SampleType energy() const noexcept {
        return static_cast<SampleType>(energy_);
    }
    [[nodiscard]] int particle_count() const noexcept { return particle_count_; }
    [[nodiscard]] SampleType event_rate() const noexcept {
        return static_cast<SampleType>(event_rate_);
    }
    [[nodiscard]] SampleType energy_decay_ms() const noexcept {
        return static_cast<SampleType>(energy_decay_ms_);
    }
    [[nodiscard]] SampleType sustain_floor() const noexcept {
        return static_cast<SampleType>(sustain_floor_);
    }
    [[nodiscard]] SampleType collision_energy_loss() const noexcept {
        return static_cast<SampleType>(collision_energy_loss_);
    }
    [[nodiscard]] std::uint64_t seed() const noexcept { return seed_; }
    [[nodiscard]] double sample_rate() const noexcept { return sample_rate_; }

    static constexpr int latency_samples() noexcept { return 0; }
    static constexpr std::size_t retained_bytes() noexcept { return 0; }

    /// Conservative samples until the decay envelope reaches its exact floor,
    /// assuming no more excite() calls. Collisions can only shorten this tail.
    [[nodiscard]] int tail_samples() const noexcept {
        if (energy_ <= 0.0)
            return 0;
        if (sustain_floor_ <= 0.0 || !(decay_coefficient_ > 0.0 && decay_coefficient_ < 1.0))
            return -1;
        if (energy_ < sustain_floor_)
            return 0;
        const double samples = std::ceil(std::log(sustain_floor_ / energy_) /
                                         std::log(decay_coefficient_));
        if (!std::isfinite(samples) ||
            samples >= static_cast<double>(std::numeric_limits<int>::max()))
            return std::numeric_limits<int>::max();
        return std::max(0, static_cast<int>(samples) + 1);
    }

private:
    static std::uint32_t derive_seed(std::uint64_t seed, std::uint64_t purpose) noexcept {
        const std::uint64_t mixed = mix64(seed, 0u, purpose);
        const auto folded = static_cast<std::uint32_t>(mixed ^ (mixed >> 32));
        return folded == 0u ? NoiseSourceT<double>::default_seed : folded;
    }

    void apply_seed() noexcept {
        event_rng_.set_seed(derive_seed(seed_, 0x4556454E54ull));
        jitter_rng_.set_seed(derive_seed(seed_, 0x4A4954544552ull));
    }

    void update_decay_coefficient() noexcept {
        decay_coefficient_ = std::exp(-1.0 / (0.001 * energy_decay_ms_ * sample_rate_));
    }

    void snap_energy() noexcept {
        if (energy_ < sustain_floor_)
            energy_ = 0.0;
    }

    NoiseSourceT<double> event_rng_;
    NoiseSourceT<double> jitter_rng_;
    double sample_rate_ = kDefaultSampleRate;
    double energy_ = 0.0;
    double event_rate_ = 400.0;
    double energy_decay_ms_ = 250.0;
    double sustain_floor_ = 0.02;
    double collision_energy_loss_ = 0.004;
    double particle_scale_ = 8.0;
    double decay_coefficient_ = 0.0;
    int particle_count_ = 64;
    std::uint64_t seed_ = kDefaultSeed;
};

using ParticleCollisionExciter = ParticleCollisionExciterT<float>;
using ParticleCollisionExciter64 = ParticleCollisionExciterT<double>;

} // namespace pulp::signal
